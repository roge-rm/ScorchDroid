# The ScorchDroid server's web container: the admin page, and the game itself
# for browsers at /play/, with the WebSocket relay that joins them to the game
# server (web-admin/app/play.py).
#
# Owns no game state. Every admin page is a rendering of what the server said
# over the control socket, and every button is one control command - so this
# container can be restarted, or left out entirely, without the game
# noticing.

# The browser game: the engine built with Emscripten and the shared UI with
# Kotlin/Wasm, the same build as ./gradlew :webApp:wasmJsBrowserDistribution,
# without the Android app (there's no Android SDK here, see
# settings.gradle.kts). The Emscripten version is the one web/engine is built
# and tested with.
FROM emscripten/emsdk:6.0.10 AS play

RUN apt-get update \
 && apt-get install -y --no-install-recommends openjdk-21-jdk-headless git \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . /src

# The submodules' .git pointers dangle in a build context, and git apply
# refuses to run beside one (see server.Dockerfile).
RUN find /src -maxdepth 3 -name .git -exec rm -rf {} +

RUN ./gradlew --no-daemon -Pscorchdroid.webOnly :webApp:wasmJsBrowserDistribution

# Gzipped ahead of time, since the game data is 87MB as it is and 39MB
# gzipped; play.py serves the .gz to any browser that takes it.
RUN cp -r web/app/build/dist/wasmJs/productionExecutable /play \
 && rm -f /play/*.map \
 && gzip -9 -k /play/*.data /play/*.wasm /play/*.js


FROM python:3.13-slim

RUN groupadd --gid 10001 scorchdroid \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin scorchdroid

WORKDIR /app

COPY web-admin/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY web-admin/app /app/app
COPY --from=play /play /app/play

ENV SCORCHDROID_CONTROL_SOCKET=/run/scorchdroid/control.sock \
    WEB_PORT=8080 \
    WEB_BIND=0.0.0.0 \
    LAN_DISCOVERY=1 \
    PLAY_DIR=/app/play

EXPOSE 8080/tcp

USER scorchdroid

# Shell form so WEB_BIND/WEB_PORT can come from the environment - with host
# networking there is no published port to change instead.
CMD ["sh", "-c", "exec uvicorn app.main:app --host \"$WEB_BIND\" --port \"$WEB_PORT\" --proxy-headers"]
