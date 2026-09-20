# The ScorchDroid server's web admin.
#
# Owns no game state. Every page is a rendering of what the server said over
# the control socket, and every button is one control command - so this
# container can be restarted, or left out entirely, without the game
# noticing.

FROM python:3.13-slim

RUN groupadd --gid 10001 scorchdroid \
 && useradd --uid 10001 --gid 10001 --no-create-home --shell /usr/sbin/nologin scorchdroid

WORKDIR /app

COPY web-admin/requirements.txt /app/requirements.txt
RUN pip install --no-cache-dir -r /app/requirements.txt

COPY web-admin/app /app/app

ENV SCORCHDROID_CONTROL_SOCKET=/run/scorchdroid/control.sock \
    WEB_PORT=8080 \
    WEB_BIND=0.0.0.0 \
    LAN_DISCOVERY=1

EXPOSE 8080/tcp

USER scorchdroid

# Shell form so WEB_BIND/WEB_PORT can come from the environment - with host
# networking there is no published port to change instead.
CMD ["sh", "-c", "exec uvicorn app.main:app --host \"$WEB_BIND\" --port \"$WEB_PORT\" --proxy-headers"]
