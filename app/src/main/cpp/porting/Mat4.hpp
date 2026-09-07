#ifndef __INCLUDE_Mat4_hpp_INCLUDE__
#define __INCLUDE_Mat4_hpp_INCLUDE__

#include <cmath>
#include <cstring>

// M6: minimal column-major 4x4 float matrix math for the new 3D renderer
// (renderer_jni.cpp) - upstream's own 3D camera/matrix code lives entirely
// in the excluded src/client rendering layer (GLW/GLEXT, fixed-function
// era), so this is new, from-scratch code rather than a port of anything.
// Column-major to match GLES's glUniformMatrix4fv(..., GL_FALSE, ...)
// expectation directly (no transpose needed).
struct Mat4 {
    float m[16];

    static Mat4 identity() {
        Mat4 r{};
        r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.0f;
        return r;
    }

    // a * b (applies b first, then a - standard OpenGL convention).
    static Mat4 multiply(const Mat4 &a, const Mat4 &b) {
        Mat4 r{};
        for (int col = 0; col < 4; col++) {
            for (int row = 0; row < 4; row++) {
                float sum = 0.0f;
                for (int k = 0; k < 4; k++) {
                    sum += a.m[k * 4 + row] * b.m[col * 4 + k];
                }
                r.m[col * 4 + row] = sum;
            }
        }
        return r;
    }

    static Mat4 translate(float x, float y, float z) {
        Mat4 r = identity();
        r.m[12] = x; r.m[13] = y; r.m[14] = z;
        return r;
    }

    static Mat4 scale(float s) {
        Mat4 r = identity();
        r.m[0] = r.m[5] = r.m[10] = s;
        return r;
    }

    // Non-uniform, for shapes whose extents differ per axis - a square
    // shield's box, for instance.
    static Mat4 scale(float x, float y, float z) {
        Mat4 r = identity();
        r.m[0] = x; r.m[5] = y; r.m[10] = z;
        return r;
    }

    // Gun elevation rotates about the model's X axis (upstream rotates
    // about X too - see ModelRendererTank::draw).
    static Mat4 rotateX(float radians) {
        Mat4 r = identity();
        float c = cosf(radians), s = sinf(radians);
        r.m[5] = c;  r.m[6] = s;
        r.m[9] = -s; r.m[10] = c;
        return r;
    }

    // Rotation about the world up (Y) axis - a tank's turret bearing.
    static Mat4 rotateY(float radians) {
        Mat4 r = identity();
        float c = cosf(radians), s = sinf(radians);
        r.m[0] = c;  r.m[2] = -s;
        r.m[8] = s;  r.m[10] = c;
        return r;
    }

    static Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ) {
        Mat4 r{};
        float f = 1.0f / tanf(fovYRadians / 2.0f);
        r.m[0] = f / aspect;
        r.m[5] = f;
        r.m[10] = (farZ + nearZ) / (nearZ - farZ);
        r.m[11] = -1.0f;
        r.m[14] = (2.0f * farZ * nearZ) / (nearZ - farZ);
        return r;
    }

    static Mat4 lookAt(
        float eyeX, float eyeY, float eyeZ,
        float centerX, float centerY, float centerZ,
        float upX, float upY, float upZ) {
        float fx = centerX - eyeX, fy = centerY - eyeY, fz = centerZ - eyeZ;
        float fLen = sqrtf(fx * fx + fy * fy + fz * fz);
        if (fLen < 1e-6f) fLen = 1e-6f;
        fx /= fLen; fy /= fLen; fz /= fLen;

        // s = f x up, normalized (right vector).
        float sx = fy * upZ - fz * upY;
        float sy = fz * upX - fx * upZ;
        float sz = fx * upY - fy * upX;
        float sLen = sqrtf(sx * sx + sy * sy + sz * sz);
        if (sLen < 1e-6f) sLen = 1e-6f;
        sx /= sLen; sy /= sLen; sz /= sLen;

        // u = s x f (recomputed up, orthogonal to both).
        float ux = sy * fz - sz * fy;
        float uy = sz * fx - sx * fz;
        float uz = sx * fy - sy * fx;

        Mat4 r = identity();
        r.m[0] = sx; r.m[4] = sy; r.m[8] = sz;
        r.m[1] = ux; r.m[5] = uy; r.m[9] = uz;
        r.m[2] = -fx; r.m[6] = -fy; r.m[10] = -fz;
        r.m[12] = -(sx * eyeX + sy * eyeY + sz * eyeZ);
        r.m[13] = -(ux * eyeX + uy * eyeY + uz * eyeZ);
        r.m[14] = (fx * eyeX + fy * eyeY + fz * eyeZ);
        return r;
    }
};

#endif  // __INCLUDE_Mat4_hpp_INCLUDE__
