// ─────────────────────────────────────────────────────────────────────────────
// voxel_field.h — CPU→GPU bridge: splat SPH particles into 3D textures each frame.
//
// raylib has no 3D-texture API, so this drops to raw GL via rlgl. Include AFTER
// <raylib.h>/<rlgl.h>. Three volumes, rebuilt every frame from the live particles:
//
//   density   R16F   64³  — mass in each voxel (the SPH kernel, splatted)
//   velTemp   RGBA16F 64³ — momentum-weighted velocity (xyz) + temperature (w)
//   occupy    R8     32³  — conservative 1/0: is gas near this cell? (skip map)
//
// The occupancy grid is HALF resolution and DILATED (any occupied fine cell marks
// its whole coarse cell + neighbours), so the ray always drops to fine steps
// BEFORE it reaches real gas — no thin sheet is ever skipped.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <raylib.h>          // Shader, SetShaderValue, SHADER_UNIFORM_INT
#include <vector>
#include <cstring>
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>       // glGenTextures/glTexImage3D/... — requires linking
                              // -framework OpenGL (see CMakeLists.txt); raylib's own
                              // GL bindings are not visible to symbols used directly.

struct VoxelField {
    // The cube now covers the FULL SPH domain (±GRID_HALF, ~3x the old ±700 edge)
    // instead of a small box around the hole, so voxel size would triple (blockier)
    // at the old N=100. Scaled N up to 160 to claw most of that back — still coarser
    // than the original ±700 box's resolution, but far less than a bare 3x. This is
    // the main cost knob if FPS needs to come back up: lower it first.
    static const int  N   = 160;         // fine grid resolution per axis
    static const int  NO  = 48;          // occupancy (coarse) resolution per axis
    float             gridMin[3];       // world corner
    float             gridSize;         // world edge length

    unsigned int texDensity = 0, texVelTemp = 0, texOccupy = 0;

    std::vector<float>         density;   // N³      (R)
    std::vector<float>         velTemp;   // N³ · 4  (RGBA)
    std::vector<unsigned char> occupy;    // NO³     (R)

    void init(float worldCorner, float worldEdge) {
        gridMin[0] = gridMin[1] = gridMin[2] = worldCorner;
        gridSize = worldEdge;
        density.assign(N*N*N, 0.f);
        velTemp.assign(N*N*N*4, 0.f);
        occupy.assign(NO*NO*NO, 0);
        texDensity = make3D(GL_R16F,    GL_RED,  N);
        texVelTemp = make3D(GL_RGBA16F, GL_RGBA, N);
        texOccupy  = make3D(GL_R8,      GL_RED,  NO);
    }

    static unsigned int make3D(GLint internal, GLenum fmt, int n) {
        unsigned int id; glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_3D, id);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
        glTexImage3D(GL_TEXTURE_3D, 0, internal, n, n, n, 0, fmt, GL_FLOAT, nullptr);
        glBindTexture(GL_TEXTURE_3D, 0);
        return id;
    }

    inline int idx(int x,int y,int z) const { return (z*N + y)*N + x; }

    // Splat every alive particle into the grids. Called once per frame, AFTER the
    // SPH step so velocities/temperatures are current.
    template <class PArray>
    void build(const PArray& ps, float uHot) {
        std::fill(density.begin(), density.end(), 0.f);
        std::fill(velTemp.begin(), velTemp.end(), 0.f);
        std::fill(occupy.begin(),  occupy.end(),  0);

        const float inv = N / gridSize;                 // world → fine cell
        const float invO = NO / gridSize;               // world → coarse cell

        for (const auto& p : ps) {
            if (!p.alive) continue;
            float gx = (p.pos.x - gridMin[0]) * inv;
            float gy = (p.pos.y - gridMin[1]) * inv;
            float gz = (p.pos.z - gridMin[2]) * inv;
            int   ix = (int)gx, iy = (int)gy, iz = (int)gz;
            if (ix<1||iy<1||iz<1||ix>=N-1||iy>=N-1||iz>=N-1) continue;

            // Deposit into the 8 surrounding cells (trilinear splat = smooth field).
            float fx=gx-ix, fy=gy-iy, fz=gz-iz;
            float temp = fminf(1.f, sqrtf(p.u / uHot));
            for (int dz=0;dz<2;dz++) for (int dy=0;dy<2;dy++) for (int dx=0;dx<2;dx++) {
                float w = (dx?fx:1-fx)*(dy?fy:1-fy)*(dz?fz:1-fz);
                int   c = idx(ix+dx, iy+dy, iz+dz);
                density[c]        += w;
                velTemp[c*4+0]    += w * p.vel.x;
                velTemp[c*4+1]    += w * p.vel.y;
                velTemp[c*4+2]    += w * p.vel.z;
                velTemp[c*4+3]    += w * temp;
            }

            // Mark the coarse occupancy cell AND its neighbours (dilation), so the
            // ray fine-steps before it reaches gas. This is the conservative skip.
            int ox=(int)((p.pos.x-gridMin[0])*invO);
            int oy=(int)((p.pos.y-gridMin[1])*invO);
            int oz=(int)((p.pos.z-gridMin[2])*invO);
            for (int dz=-1;dz<=1;dz++) for (int dy=-1;dy<=1;dy++) for (int dx=-1;dx<=1;dx++){
                int nx=ox+dx, ny=oy+dy, nz=oz+dz;
                if (nx<0||ny<0||nz<0||nx>=NO||ny>=NO||nz>=NO) continue;
                occupy[(nz*NO+ny)*NO+nx] = 255;
            }
        }

        // Normalise velocity/temperature by mass so they're averages, not sums.
        for (int c=0;c<N*N*N;c++) {
            float m = density[c];
            if (m > 1e-5f) { velTemp[c*4+0]/=m; velTemp[c*4+1]/=m; velTemp[c*4+2]/=m; velTemp[c*4+3]/=m; }
        }

        upload();
    }

    void upload() {
        glBindTexture(GL_TEXTURE_3D, texDensity);
        glTexSubImage3D(GL_TEXTURE_3D,0,0,0,0,N,N,N,GL_RED, GL_FLOAT, density.data());
        glBindTexture(GL_TEXTURE_3D, texVelTemp);
        glTexSubImage3D(GL_TEXTURE_3D,0,0,0,0,N,N,N,GL_RGBA,GL_FLOAT, velTemp.data());
        glBindTexture(GL_TEXTURE_3D, texOccupy);
        glTexSubImage3D(GL_TEXTURE_3D,0,0,0,0,NO,NO,NO,GL_RED,GL_UNSIGNED_BYTE, occupy.data());
        glBindTexture(GL_TEXTURE_3D, 0);
    }

    // Bind the three volumes to texture units 1/2/3 for volume.fs.
    void bind(Shader sh, int locDens, int locVel, int locOcc) const {
        glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_3D, texDensity);
        glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_3D, texVelTemp);
        glActiveTexture(GL_TEXTURE3); glBindTexture(GL_TEXTURE_3D, texOccupy);
        int u1=1,u2=2,u3=3;
        SetShaderValue(sh, locDens, &u1, SHADER_UNIFORM_INT);
        SetShaderValue(sh, locVel,  &u2, SHADER_UNIFORM_INT);
        SetShaderValue(sh, locOcc,  &u3, SHADER_UNIFORM_INT);
        glActiveTexture(GL_TEXTURE0);
    }

    void unload() {
        if (texDensity) glDeleteTextures(1,&texDensity);
        if (texVelTemp) glDeleteTextures(1,&texVelTemp);
        if (texOccupy)  glDeleteTextures(1,&texOccupy);
    }
};