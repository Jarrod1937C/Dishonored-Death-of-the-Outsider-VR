#pragma once
#include <cmath>
#include <cstring>

//! Small helpers shared by the camera hooks.
//! OpenXR: +X right, +Y up, -Z forward (meters).   id Tech: +X forward, +Y left, +Z up (Void: meters).
namespace VRMath
{
    inline void XrToId(const float v[3], float out[3]) { out[0] = -v[2]; out[1] = -v[0]; out[2] = v[1]; }

    //! Basis vectors of a quaternion (x,y,z,w) in XR space: fwd(-Z), right(+X), up(+Y).
    inline void QuatBasis(const float q[4], float fwd[3], float right[3], float up[3])
    {
        float x = q[0], y = q[1], z = q[2], w = q[3];
        float m00 = 1 - 2 * (y * y + z * z), m01 = 2 * (x * y - z * w), m02 = 2 * (x * z + y * w);
        float m10 = 2 * (x * y + z * w), m11 = 1 - 2 * (x * x + z * z), m12 = 2 * (y * z - x * w);
        float m20 = 2 * (x * z - y * w), m21 = 2 * (y * z + x * w), m22 = 1 - 2 * (x * x + y * y);
        right[0] = m00; right[1] = m10; right[2] = m20;
        up[0] = m01;    up[1] = m11;    up[2] = m21;
        fwd[0] = -m02;  fwd[1] = -m12;  fwd[2] = -m22;
    }

    //! Composes the game's (body) yaw taken from `axis` with an HMD pose:
    //! axis <- yaw(body) * hmdRotation ; org <- org + yaw(body) * (hmdPos - refPos) * scale.
    //! axis is an idMat3 (rows fwd/left/up). hmdPos/refPos/quat are XR-space.
    inline void ComposeHead(const float hmdPos[3], const float hmdQuat[4], const float refPos[3], float scale,
                            float org[3], float axis[9])
    {
        float yaw = atan2f(axis[1], axis[0]);
        float cy = cosf(yaw), sy = sinf(yaw);
        auto toWorld = [&](const float l[3], float out[3]) {
            out[0] = l[0] * cy - l[1] * sy; out[1] = l[0] * sy + l[1] * cy; out[2] = l[2]; };
        float fx[3], rx[3], ux[3]; QuatBasis(hmdQuat, fx, rx, ux);
        float fl[3], rl[3], ul[3]; XrToId(fx, fl); XrToId(rx, rl); XrToId(ux, ul);
        float fw[3], rw[3], uw[3]; toWorld(fl, fw); toWorld(rl, rw); toWorld(ul, uw);
        axis[0] = fw[0]; axis[1] = fw[1]; axis[2] = fw[2];
        axis[3] = -rw[0]; axis[4] = -rw[1]; axis[5] = -rw[2];
        axis[6] = uw[0]; axis[7] = uw[1]; axis[8] = uw[2];
        float dxr[3] = { hmdPos[0] - refPos[0], hmdPos[1] - refPos[1], hmdPos[2] - refPos[2] };
        float dl[3]; XrToId(dxr, dl); float dw[3]; toWorld(dl, dw);
        org[0] += dw[0] * scale; org[1] += dw[1] * scale; org[2] += dw[2] * scale;
    }

    //! Quaternions (x,y,z,w): o = a * b, o = conj(a) * b, v' = q v q*.
    inline void QMul(const float a[4], const float b[4], float o[4])
    {
        const float r[4] = { a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1], a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
                             a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3], a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2] };
        memcpy(o, r, 16);
    }
    inline void QConjMul(const float a[4], const float b[4], float o[4]) { const float c[4] = { -a[0], -a[1], -a[2], a[3] }; QMul(c, b, o); }
    inline void QRotate(const float q[4], const float v[3], float o[3])
    {
        const float u[3] = { q[0], q[1], q[2] }, w = q[3];
        const float c1[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
        const float c2[3] = { u[1] * c1[2] - u[2] * c1[1], u[2] * c1[0] - u[0] * c1[2], u[0] * c1[1] - u[1] * c1[0] };
        for (int i = 0; i < 3; i++) o[i] = v[i] + 2 * (w * c1[i] + c2[i]);
    }

    //! An idMat3 (rows fwd/left/up, world) turned by a rotation given in its OWN XR-local frame (x right, y up, -z fwd):
    //! what a canted eye's axis is when `axis` is the head's and `rel` the eye's rotation relative to the head.
    inline void TurnAxisLocal(const float axis[9], const float rel[4], float out[9])
    {
        const float xf[3] = { 0, 0, -1 }, xl[3] = { -1, 0, 0 }, xu[3] = { 0, 1, 0 };   // fwd, left, up in XR local
        const float* in[3] = { xf, xl, xu };
        for (int r = 0; r < 3; r++)
        {
            float v[3], l[3]; QRotate(rel, in[r], v); XrToId(v, l);                     // that basis vector, in id local
            for (int k = 0; k < 3; k++) out[r * 3 + k] = l[0] * axis[k] + l[1] * axis[3 + k] + l[2] * axis[6 + k];
        }
    }

    //! 4x4 inverse (row-major), false when singular.
    inline bool Inverse4(const float* m, float* o)
    {
        float inv[16];
        inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
        inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
        inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
        inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
        inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
        inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
        inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
        inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
        inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
        inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
        inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
        inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
        inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
        inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
        inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
        inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
        const double det = (double)m[0] * inv[0] + (double)m[1] * inv[4] + (double)m[2] * inv[8] + (double)m[3] * inv[12];
        if (fabs(det) < 1e-20) return false;
        for (int i = 0; i < 16; i++) o[i] = (float)(inv[i] / det);
        return true;
    }
}
