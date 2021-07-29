#include <math.h>
#include "../sailfish.h"


// ============================ COMPAT ========================================
// ============================================================================
#ifdef __ROCM__
#include <hip/hip_runtime.h>
#endif

#if !defined(__NVCC__) && !defined(__ROCM__)
#define __device__
#define __host__
#define EXTERN_C
#else
#define EXTERN_C extern "C"
#endif


// ============================ PHYSICS =======================================
// ============================================================================
#define NCONS 4
#define PLM_THETA 1.5
#define GAMMA_LAW_INDEX (5.0 / 3.0)

// ============================ MATH ==========================================
// ============================================================================
#define real double
#define min2(a, b) ((a) < (b) ? (a) : (b))
#define max2(a, b) ((a) > (b) ? (a) : (b))
#define min3(a, b, c) min2(a, min2(b, c))
#define max3(a, b, c) max2(a, max2(b, c))
#define sign(x) copysign(1.0, x)
#define minabs(a, b, c) min3(fabs(a), fabs(b), fabs(c))

static __host__ __device__ real plm_gradient_scalar(real yl, real y0, real yr)
{
    real a = (y0 - yl) * PLM_THETA;
    real b = (yr - yl) * 0.5;
    real c = (yr - y0) * PLM_THETA;
    return 0.25 * fabs(sign(a) + sign(b)) * (sign(a) + sign(c)) * minabs(a, b, c);
}

static __host__ __device__ void plm_gradient(real *yl, real *y0, real *yr, real *g)
{
    for (int q = 0; q < NCONS; ++q)
    {
        g[q] = plm_gradient_scalar(yl[q], y0[q], yr[q]);
    }
}


// ============================ GRAVITY =======================================
// ============================================================================
//static __host__ __device__ real gravitational_potential(
//    struct PointMass *masses,
//    int num_masses,
//    real x1,
//    real y1,
//    real h)
//{
//    real phi = 0.0;

//    for (int p = 0; p < num_masses; ++p)
//    {
//        real x0 = masses[p].x;
//        real y0 = masses[p].y;
//        real mp = masses[p].mass;
//        //real rs = masses[p].radius;

//        real dx = x1 - x0;
//        real dy = y1 - y0;
//        real r2 = dx * dx + dy * dy;
//        real r2_soft = r2 + (0.5 * h) * (0.5 * h);

//        phi -= mp / sqrt(r2_soft);
//    }
//    return phi;
//}

static __host__ __device__ real disk_height(
    struct PointMass *masses,
    int num_masses,
    real x1,
    real y1,
    real *prim)
{
    real omegatilde2 = 0.0;
    for (int p = 0; p < num_masses; ++p)
    {
        real x0 = masses[p].x;
        real y0 = masses[p].y;
        real mp = masses[p].mass;

        real dx = x1 - x0;
        real dy = y1 - y0;
        real r2 = dx * dx + dy * dy + 1e-12;
        real r  = sqrt(r2);
        omegatilde2 += mp / r2 / r;
    }
    real sigma = prim[0];
    real pres  = prim[3];

    return sqrt(pres / sigma) / sqrt(omegatilde2);
}

static __host__ __device__ void point_mass_source_term(
    struct PointMass *mass,
    real x1,
    real y1,
    real dt,
    real *prim,
    real h,
    real *delta_cons)
{
    real x0 = mass->x;
    real y0 = mass->y;
    real mp = mass->mass;
    real sigma = prim[0];
    real pres  = prim[3];
    real gamma = GAMMA_LAW_INDEX;
    real eps = pres / (gamma - 1.0) / sigma;

    real dx = x1 - x0;
    real dy = y1 - y0;
    real r2 = dx * dx + dy * dy;
    real rs = 0.5 * h;
    real r2_soft = r2 + rs * rs;
    real dr = sqrt(r2);
    real mag = sigma * mp / r2_soft;
    real fx = -mag * dx / dr;
    real fy = -mag * dy / dr;
    real sink_rate = 0.0;

    if (dr < 4.0 * rs)
    {
        sink_rate = mass->rate * exp(-pow(dr / rs, 4.0));
    }
    real mdot = sigma * sink_rate * -1.0;

    switch (mass->model) {
        case AccelerationFree:
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * mdot * prim[1] + dt * fx;
            delta_cons[2] = dt * mdot * prim[2] + dt * fy;
            delta_cons[3] = dt * (mdot * eps + 0.5 * mdot * (prim[1] * prim[1] + prim[2] * prim[2])) + dt * (fx * prim[1] + fy * prim[2]);
            break;
        case TorqueFree: {
            real vx        = prim[1];
            real vy        = prim[2];
            real vx0       = mass->vx;
            real vy0       = mass->vy;
            real rhatx     = dx / dr;
            real rhaty     = dy / dr;
            real dvdotrhat = (vx - vx0) * rhatx + (vy - vy0) * rhaty;
            real vxstar    = dvdotrhat * rhatx + vx0;
            real vystar    = dvdotrhat * rhaty + vy0;
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * mdot * vxstar + dt * fx;
            delta_cons[2] = dt * mdot * vystar + dt * fy;
            delta_cons[3] = dt * (mdot * eps + 0.5 * mdot * (vxstar * vxstar + vystar * vystar)) + dt * (fx * prim[1] + fy * prim[2]);
            break;
        }
        case ForceFree:
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * fx;
            delta_cons[2] = dt * fy;
            delta_cons[3] = dt * (fx * prim[1] + fy * prim[2]);
            break;
        default:
            delta_cons[0] = 0.0;
            delta_cons[1] = 0.0;
            delta_cons[2] = 0.0;
            delta_cons[3] = 0.0;
            break;
    }
}

static __host__ __device__ void point_masses_source_term(
    struct PointMass* masses,
    int num_masses,
    real x1,
    real y1,
    real dt,
    real *prim,
    real h,
    real *cons)
{
    for (int p = 0; p < num_masses; ++p)
    {
        real delta_cons[NCONS];
        point_mass_source_term(&masses[p], x1, y1, dt, prim, h, delta_cons);

        for (int q = 0; q < NCONS; ++q)
        {
            cons[q] += delta_cons[q];
        }
    }
}


// ============================ EOS AND BUFFER ================================
// ============================================================================
static __host__ __device__ real sound_speed_squared(
    struct EquationOfState *eos,
    real *prim)
{
    switch (eos->type)
    {
        case GammaLaw:
            return prim[3] / prim[0] * GAMMA_LAW_INDEX;
        default:
            return 1.0; // WARNING
    }
}

static __host__ __device__ void buffer_source_term(
    struct BufferZone *buffer,
    real xc,
    real yc,
    real dt,
    real *cons)
{
    switch (buffer->type)
    {
        case None:
        {
            break;
        }
        case Keplerian:
        {
            real rc = sqrt(xc * xc + yc * yc);
            real surface_density = buffer->keplerian.surface_density;
            real surface_pressure = buffer->keplerian.surface_pressure;
            real central_mass = buffer->keplerian.central_mass;
            real driving_rate = buffer->keplerian.driving_rate;
            real outer_radius = buffer->keplerian.outer_radius;
            real onset_width = buffer->keplerian.onset_width;
            real onset_radius = outer_radius - onset_width;

            if (rc > onset_radius)
            {
                real pf = surface_density * sqrt(central_mass / rc);
                real px = pf * (-yc / rc);
                real py = pf * ( xc / rc);
                real kinetic_energy = 0.5 * (px * px + py * py) / surface_density;
                real energy = surface_pressure / (GAMMA_LAW_INDEX - 1.0) + kinetic_energy;
                real u0[NCONS] = {surface_density, px, py, energy};

                real omega_outer = sqrt(central_mass / pow(onset_radius, 3.0));
                real buffer_rate = driving_rate * omega_outer * max2(rc, 1.0);

                for (int q = 0; q < NCONS; ++q)
                {
                    cons[q] -= (cons[q] - u0[q]) * buffer_rate * dt;
                }
            }
            break;
        }
    }
}

static __host__ __device__ void shear_strain(const real *gx, const real *gy, real dx, real dy, real *s)
{
    real sxx = 4.0 / 3.0 * gx[1] / dx - 2.0 / 3.0 * gy[2] / dy;
    real syy =-2.0 / 3.0 * gx[1] / dx + 4.0 / 3.0 * gy[2] / dy;
    real sxy = 1.0 / 1.0 * gx[2] / dx + 1.0 / 1.0 * gy[1] / dy;
    real syx = sxy;
    s[0] = sxx;
    s[1] = sxy;
    s[2] = syx;
    s[3] = syy;
}


// ============================ HYDRO =========================================
// ============================================================================
static __host__ __device__ void cooling_term(real cooling_coefficient, real dt, real *prim, real *cons)
{
    real gamma = GAMMA_LAW_INDEX;
    real sigma = prim[0];
    real eps   = prim[3] / (gamma - 1.0);
    real eps_cooled = eps * pow(1.0 + 3.0 * cooling_coefficient / pow(sigma, 2.0) * pow(eps,3) * dt, -1.0 / 3.0);

    cons[3] += sigma * (eps_cooled - eps);
}

static __host__ __device__ void conserved_to_primitive(const real *cons, real *prim, real velocity_ceiling, real density_floor, real pressure_floor)
{
    real gamma = GAMMA_LAW_INDEX;
    real rho = max2(cons[0], density_floor);
    real px = cons[1];
    real py = cons[2];
    real vx = sign(px) * min2(fabs(px / rho), velocity_ceiling);
    real vy = sign(py) * min2(fabs(py / rho), velocity_ceiling);
    real pres = max2( (cons[3] - 0.5 * rho * (vx * vx + vy * vy)) * (gamma - 1.0), pressure_floor );

    prim[0] = rho;
    prim[1] = vx;
    prim[2] = vy;
    prim[3] = pres;
}

static __host__ __device__ void primitive_to_conserved(const real *prim, real *cons)
{
    real gamma = GAMMA_LAW_INDEX;
    real rho = prim[0];
    real vx = prim[1];
    real vy = prim[2];
    real pres = prim[3];
    real px = vx * rho;
    real py = vy * rho;
    real en = pres / (gamma - 1.0) + 0.5 * rho * (vx * vx + vy * vy);

    cons[0] = rho;
    cons[1] = px;
    cons[2] = py;
    cons[3] = en;
}

static __host__ __device__ real primitive_to_velocity(const real *prim, int direction)
{
    switch (direction)
    {
        case 0: return prim[1];
        case 1: return prim[2];
        default: return 0.0;
    }
}

static __host__ __device__ void primitive_to_flux(
    const real *prim,
    const real *cons,
    real *flux,
    int direction)
{
    real vn = primitive_to_velocity(prim, direction);
    real pressure = prim[3];

    flux[0] = vn * cons[0];
    flux[1] = vn * cons[1] + pressure * (direction == 0);
    flux[2] = vn * cons[2] + pressure * (direction == 1);
    flux[3] = vn * (cons[3] + pressure);
}

static __host__ __device__ void primitive_to_outer_wavespeeds(
    const real *prim,
    real *wavespeeds,
    real cs2,
    int direction)
{
    real cs = sqrt(cs2);
    real vn = primitive_to_velocity(prim, direction);
    wavespeeds[0] = vn - cs;
    wavespeeds[1] = vn + cs;
}

static __host__ __device__ real primitive_max_wavespeed(const real *prim, real cs2)
{
    real cs = sqrt(cs2);
    real vx = prim[1];
    real vy = prim[2];
    real ax = max2(fabs(vx - cs), fabs(vx + cs));
    real ay = max2(fabs(vy - cs), fabs(vy + cs));
    return max2(ax, ay);
}

static __host__ __device__ void riemann_hlle(const real *pl, const real *pr, real *flux, real cs2, int direction)
{
    real ul[NCONS];
    real ur[NCONS];
    real fl[NCONS];
    real fr[NCONS];
    real al[2];
    real ar[2];

    primitive_to_conserved(pl, ul);
    primitive_to_conserved(pr, ur);
    primitive_to_flux(pl, ul, fl, direction);
    primitive_to_flux(pr, ur, fr, direction);
    primitive_to_outer_wavespeeds(pl, al, cs2, direction);
    primitive_to_outer_wavespeeds(pr, ar, cs2, direction);

    const real am = min3(0.0, al[0], ar[0]);
    const real ap = max3(0.0, al[1], ar[1]);

    for (int q = 0; q < NCONS; ++q)
    {
        flux[q] = (fl[q] * ap - fr[q] * am - (ul[q] - ur[q]) * ap * am) / (ap - am);
    }
}


// ============================ PATCH =========================================
// ============================================================================
#define FOR_EACH(p) \
    for (int i = p.start[0]; i < p.start[0] + p.count[0]; ++i) \
    for (int j = p.start[1]; j < p.start[1] + p.count[1]; ++j)
#define FOR_EACH_OMP(p) \
_Pragma("omp parallel for") \
    for (int i = p.start[0]; i < p.start[0] + p.count[0]; ++i) \
    for (int j = p.start[1]; j < p.start[1] + p.count[1]; ++j)
#define CONTAINS(p, q) \
        (p.start[0] <= q.start[0] && p.start[0] + p.count[0] >= q.start[0] + q.count[0]) && \
        (p.start[1] <= q.start[1] && p.start[1] + p.count[1] >= q.start[1] + q.count[1])
#define GET(p, i, j) (p.data + p.jumps[0] * ((i) - p.start[0]) + p.jumps[1] * ((j) - p.start[1]))
#define ELEMENTS(p) (p.count[0] * p.count[1] * p.num_fields)
#define BYTES(p) (ELEMENTS(p) * sizeof(real))

struct Patch
{
    int start[2];
    int count[2];
    int jumps[2];
    int num_fields;
    real *data;
};

static struct Patch patch(struct Mesh mesh, int num_fields, int num_guard, real *data)
{
    struct Patch patch;
    patch.start[0] = -num_guard;
    patch.start[1] = -num_guard;
    patch.count[0] = mesh.ni + 2 * num_guard;
    patch.count[1] = mesh.nj + 2 * num_guard;
    patch.jumps[0] = num_fields * patch.count[1];
    patch.jumps[1] = num_fields;
    patch.num_fields = num_fields;
    patch.data = data;
    return patch;
}


// ============================ SCHEME ========================================
// ============================================================================
static __host__ __device__ void primitive_to_conserved_zone(
        struct Patch primitive,
        struct Patch conserved,
        int i,
        int j)
{
    real *p = GET(primitive, i, j);
    real *u = GET(conserved, i, j);
    primitive_to_conserved(p, u);
}

static __host__ __device__ void advance_rk_zone(
    struct Mesh mesh,
    struct Patch conserved_rk,
    struct Patch primitive_rd,
    struct Patch primitive_wr,
    struct EquationOfState eos,
    struct BufferZone buffer,
    struct PointMass *masses,
    int num_masses,
    real alpha,
    real a,
    real dt,
    real velocity_ceiling,
    real cooling_coefficient,
    real density_floor,
    real pressure_floor,
    int i,
    int j)
{
    real dx = mesh.dx;
    real dy = mesh.dy;
    //real xl = mesh.x0 + (i + 0.0) * dx;
    real xc = mesh.x0 + (i + 0.5) * dx;
    //real xr = mesh.x0 + (i + 1.0) * dx;
    //real yl = mesh.y0 + (j + 0.0) * dy;
    real yc = mesh.y0 + (j + 0.5) * dy;
    //real yr = mesh.y0 + (j + 1.0) * dy;

    // ------------------------------------------------------------------------
    //                 tj
    //
    //      +-------+-------+-------+
    //      |       |       |       |
    //      |  lr   |  rj   |   rr  |
    //      |       |       |       |
    //      +-------+-------+-------+
    //      |       |       |       |
    //  ki  |  li  -|+  c  -|+  ri  |  ti
    //      |       |       |       |
    //      +-------+-------+-------+
    //      |       |       |       |
    //      |  ll   |  lj   |   rl  |
    //      |       |       |       |
    //      +-------+-------+-------+
    //
    //                 kj
    // ------------------------------------------------------------------------
    real *un = GET(conserved_rk, i, j);
    real *pcc = GET(primitive_rd, i, j);
    real *pli = GET(primitive_rd, i - 1, j);
    real *pri = GET(primitive_rd, i + 1, j);
    real *plj = GET(primitive_rd, i, j - 1);
    real *prj = GET(primitive_rd, i, j + 1);
    real *pki = GET(primitive_rd, i - 2, j);
    real *pti = GET(primitive_rd, i + 2, j);
    real *pkj = GET(primitive_rd, i, j - 2);
    real *ptj = GET(primitive_rd, i, j + 2);
    real *pll = GET(primitive_rd, i - 1, j - 1);
    real *plr = GET(primitive_rd, i - 1, j + 1);
    real *prl = GET(primitive_rd, i + 1, j - 1);
    real *prr = GET(primitive_rd, i + 1, j + 1);

    real plip[NCONS];
    real plim[NCONS];
    real prip[NCONS];
    real prim[NCONS];
    real pljp[NCONS];
    real pljm[NCONS];
    real prjp[NCONS];
    real prjm[NCONS];

    real gxli[NCONS];
    real gxri[NCONS];
    real gyli[NCONS];
    real gyri[NCONS];
    real gxlj[NCONS];
    real gxrj[NCONS];
    real gylj[NCONS];
    real gyrj[NCONS];
    real gxcc[NCONS];
    real gycc[NCONS];

    plm_gradient(pki, pli, pcc, gxli);
    plm_gradient(pli, pcc, pri, gxcc);
    plm_gradient(pcc, pri, pti, gxri);
    plm_gradient(pkj, plj, pcc, gylj);
    plm_gradient(plj, pcc, prj, gycc);
    plm_gradient(pcc, prj, ptj, gyrj);
    plm_gradient(pll, pli, plr, gyli);
    plm_gradient(prl, pri, prr, gyri);
    plm_gradient(pll, plj, prl, gxlj);
    plm_gradient(plr, prj, prr, gxrj);

    for (int q = 0; q < NCONS; ++q)
    {
        plim[q] = pli[q] + 0.5 * gxli[q];
        plip[q] = pcc[q] - 0.5 * gxcc[q];
        prim[q] = pcc[q] + 0.5 * gxcc[q];
        prip[q] = pri[q] - 0.5 * gxri[q];

        pljm[q] = plj[q] + 0.5 * gylj[q];
        pljp[q] = pcc[q] - 0.5 * gycc[q];
        prjm[q] = pcc[q] + 0.5 * gycc[q];
        prjp[q] = prj[q] - 0.5 * gyrj[q];
    }

    real fli[NCONS];
    real fri[NCONS];
    real flj[NCONS];
    real frj[NCONS];
    real ucc[NCONS];

    real cs2li = sound_speed_squared(&eos, pli);
    real cs2ri = sound_speed_squared(&eos, pri);
    real cs2lj = sound_speed_squared(&eos, plj);
    real cs2rj = sound_speed_squared(&eos, prj);

    riemann_hlle(plim, plip, fli, cs2li, 0);
    riemann_hlle(prim, prip, fri, cs2ri, 0);
    riemann_hlle(pljm, pljp, flj, cs2lj, 1);
    riemann_hlle(prjm, prjp, frj, cs2rj, 1);

    real sli[4];
    real sri[4];
    real slj[4];
    real srj[4];
    real scc[4];

    shear_strain(gxli, gyli, dx, dy, sli);
    shear_strain(gxri, gyri, dx, dy, sri);
    shear_strain(gxlj, gylj, dx, dy, slj);
    shear_strain(gxrj, gyrj, dx, dy, srj);
    shear_strain(gxcc, gycc, dx, dy, scc);

    real h = disk_height(masses, num_masses, xc, yc, pcc);
    real cs2cc = sound_speed_squared(&eos, pcc);
    real nu = alpha * h * sqrt(cs2cc); // Warning: nu is centered.

    fli[1] -= 0.5 * nu * (pli[0] * sli[0] + pcc[0] * scc[0]); // x-x
    fli[2] -= 0.5 * nu * (pli[0] * sli[1] + pcc[0] * scc[1]); // x-y
    fri[1] -= 0.5 * nu * (pcc[0] * scc[0] + pri[0] * sri[0]); // x-x
    fri[2] -= 0.5 * nu * (pcc[0] * scc[1] + pri[0] * sri[1]); // x-y
    flj[1] -= 0.5 * nu * (plj[0] * slj[2] + pcc[0] * scc[2]); // y-x
    flj[2] -= 0.5 * nu * (plj[0] * slj[3] + pcc[0] * scc[3]); // y-y
    frj[1] -= 0.5 * nu * (pcc[0] * scc[2] + prj[0] * srj[2]); // y-x
    frj[2] -= 0.5 * nu * (pcc[0] * scc[3] + prj[0] * srj[3]); // y-y

    fli[3] -= 0.5 * nu * (pli[0] * sli[0] * pli[1] + pcc[0] * scc[0] * pcc[1]); // v^x \tau^x_x
    fri[3] -= 0.5 * nu * (pcc[0] * scc[0] * pcc[1] + pri[0] * sri[0] * pri[1]);
    fli[3] -= 0.5 * nu * (pli[0] * sli[1] * pli[2] + pcc[0] * scc[1] * pcc[2]); // v^y \tau^x_y
    fri[3] -= 0.5 * nu * (pcc[0] * scc[1] * pcc[2] + pri[0] * sri[1] * pri[2]);
    flj[3] -= 0.5 * nu * (plj[0] * slj[2] * plj[1] + pcc[0] * scc[2] * pcc[1]); // v^x \tau^y_x
    frj[3] -= 0.5 * nu * (pcc[0] * scc[2] * pcc[1] + prj[0] * srj[2] * prj[1]);
    flj[3] -= 0.5 * nu * (plj[0] * slj[3] * plj[2] + pcc[0] * scc[3] * pcc[2]); // v^y \tau^y_y
    frj[3] -= 0.5 * nu * (pcc[0] * scc[3] * pcc[2] + prj[0] * srj[3] * prj[2]);

    primitive_to_conserved(pcc, ucc);
    buffer_source_term(&buffer, xc, yc, dt, ucc);
    point_masses_source_term(masses, num_masses, xc, yc, dt, pcc, h, ucc);
    cooling_term(cooling_coefficient, dt, pcc, ucc);

    for (int q = 0; q < NCONS; ++q)
    {
        ucc[q] -= ((fri[q] - fli[q]) / dx + (frj[q] - flj[q]) / dy) * dt;
        ucc[q] = (1.0 - a) * ucc[q] + a * un[q];
    }
    real *pout = GET(primitive_wr, i, j);
    conserved_to_primitive(ucc, pout, velocity_ceiling, density_floor, pressure_floor);
}

static __host__ __device__ void advance_rk_zone_inviscid(
    struct Mesh mesh,
    struct Patch conserved_rk,
    struct Patch primitive_rd,
    struct Patch primitive_wr,
    struct EquationOfState eos,
    struct BufferZone buffer,
    struct PointMass *masses,
    int num_masses,
    real a,
    real dt,
    real velocity_ceiling,
    real cooling_coefficient,
    real density_floor,
    real pressure_floor,
    int i,
    int j)
{
    real dx = mesh.dx;
    real dy = mesh.dy;
    //real xl = mesh.x0 + (i + 0.0) * dx;
    real xc = mesh.x0 + (i + 0.5) * dx;
    //real xr = mesh.x0 + (i + 1.0) * dx;
    //real yl = mesh.y0 + (j + 0.0) * dy;
    real yc = mesh.y0 + (j + 0.5) * dy;
    //real yr = mesh.y0 + (j + 1.0) * dy;

    real *un = GET(conserved_rk, i, j);
    real *pcc = GET(primitive_rd, i, j);
    real *pli = GET(primitive_rd, i - 1, j);
    real *pri = GET(primitive_rd, i + 1, j);
    real *plj = GET(primitive_rd, i, j - 1);
    real *prj = GET(primitive_rd, i, j + 1);
    real *pki = GET(primitive_rd, i - 2, j);
    real *pti = GET(primitive_rd, i + 2, j);
    real *pkj = GET(primitive_rd, i, j - 2);
    real *ptj = GET(primitive_rd, i, j + 2);

    real plip[NCONS];
    real plim[NCONS];
    real prip[NCONS];
    real prim[NCONS];
    real pljp[NCONS];
    real pljm[NCONS];
    real prjp[NCONS];
    real prjm[NCONS];

    real gxli[NCONS];
    real gxri[NCONS];
    real gylj[NCONS];
    real gyrj[NCONS];
    real gxcc[NCONS];
    real gycc[NCONS];

    plm_gradient(pki, pli, pcc, gxli);
    plm_gradient(pli, pcc, pri, gxcc);
    plm_gradient(pcc, pri, pti, gxri);
    plm_gradient(pkj, plj, pcc, gylj);
    plm_gradient(plj, pcc, prj, gycc);
    plm_gradient(pcc, prj, ptj, gyrj);

    for (int q = 0; q < NCONS; ++q)
    {
        plim[q] = pli[q] + 0.5 * gxli[q];
        plip[q] = pcc[q] - 0.5 * gxcc[q];
        prim[q] = pcc[q] + 0.5 * gxcc[q];
        prip[q] = pri[q] - 0.5 * gxri[q];

        pljm[q] = plj[q] + 0.5 * gylj[q];
        pljp[q] = pcc[q] - 0.5 * gycc[q];
        prjm[q] = pcc[q] + 0.5 * gycc[q];
        prjp[q] = prj[q] - 0.5 * gyrj[q];
    }

    real fli[NCONS];
    real fri[NCONS];
    real flj[NCONS];
    real frj[NCONS];
    real ucc[NCONS];

    real cs2li = sound_speed_squared(&eos, pli);
    real cs2ri = sound_speed_squared(&eos, pri);
    real cs2lj = sound_speed_squared(&eos, plj);
    real cs2rj = sound_speed_squared(&eos, prj);

    riemann_hlle(plim, plip, fli, cs2li, 0);
    riemann_hlle(prim, prip, fri, cs2ri, 0);
    riemann_hlle(pljm, pljp, flj, cs2lj, 1);
    riemann_hlle(prjm, prjp, frj, cs2rj, 1);

    real h = disk_height(masses, num_masses, xc, yc, pcc);
    primitive_to_conserved(pcc, ucc);
    buffer_source_term(&buffer, xc, yc, dt, ucc);
    point_masses_source_term(masses, num_masses, xc, yc, dt, pcc, h, ucc);
    cooling_term(cooling_coefficient, dt, pcc, ucc);

    for (int q = 0; q < NCONS; ++q)
    {
        ucc[q] -= ((fri[q] - fli[q]) / dx + (frj[q] - flj[q]) / dy) * dt;
        ucc[q] = (1.0 - a) * ucc[q] + a * un[q];
    }
    real *pout = GET(primitive_wr, i, j);
    conserved_to_primitive(ucc, pout, velocity_ceiling, density_floor, pressure_floor);
}

static __host__ __device__ void wavespeed_zone(
    struct EquationOfState eos,
    struct Patch primitive,
    struct Patch wavespeed,
    int i,
    int j)
{
    real *pc = GET(primitive, i, j);
    real cs2 = sound_speed_squared(&eos, pc);
    real a = primitive_max_wavespeed(pc, cs2);
    GET(wavespeed, i, j)[0] = a;
}


// ============================ KERNELS =======================================
// ============================================================================
#if defined(__NVCC__) || defined(__ROCM__)

static void __global__ primitive_to_conserved_kernel(
    struct Mesh mesh,
    struct Patch primitive,
    struct Patch conserved)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        primitive_to_conserved_zone(primitive, conserved, i, j);
    }
}

static void __global__ advance_rk_kernel(
    struct Mesh mesh,
    struct Patch conserved_rk,
    struct Patch primitive_rd,
    struct Patch primitive_wr,
    struct EquationOfState eos,
    struct BufferZone buffer,
    struct PointMass *masses,
    int num_masses,
    real alpha,
    real a,
    real dt,
    real velocity_ceiling,
    real cooling_coefficient)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        advance_rk_zone(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, nu, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
    }
}

static void __global__ advance_rk_kernel_inviscid(
    struct Mesh mesh,
    struct Patch conserved_rk,
    struct Patch primitive_rd,
    struct Patch primitive_wr,
    struct EquationOfState eos,
    struct BufferZone buffer,
    struct PointMass *masses,
    int num_masses,
    real a,
    real dt,
    real velocity_ceiling,
    real cooling_coefficient)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        advance_rk_zone_inviscid(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
    }
}

static void __global__ wavespeed_kernel(
    struct Mesh mesh,
    struct EquationOfState eos,
    struct Patch primitive,
    struct Patch wavespeed,
    struct PointMass *masses,
    int num_masses)
{
    int i = threadIdx.y + blockIdx.y * blockDim.y;
    int j = threadIdx.x + blockIdx.x * blockDim.x;

    if (i < mesh.ni && j < mesh.nj)
    {
        wavespeed_zone(eos, primitive, wavespeed, i, j);
    }
}

#endif


// ============================ PUBLIC API ====================================
// ============================================================================


/**
 * Converts an array of primitive data to an array of conserved data. The
 * array index space must follow the descriptions below.
 * @param mesh               The mesh [ni,     nj]
 * @param primitive_ptr[in]  [-2, -2] [ni + 4, nj + 4] [3]
 * @param conserved_ptr[out] [ 0,  0] [ni,     nj]     [3]
 * @param mode               The execution mode
 */
EXTERN_C void euler2d_primitive_to_conserved(
    struct Mesh mesh,
    real *primitive_ptr,
    real *conserved_ptr,
    enum ExecutionMode mode)
{
    struct Patch primitive = patch(mesh, NCONS, 2, primitive_ptr);
    struct Patch conserved = patch(mesh, NCONS, 0, conserved_ptr);

    switch (mode) {
        case CPU: {
            FOR_EACH(conserved) {
                primitive_to_conserved_zone(primitive, conserved, i, j);
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            FOR_EACH_OMP(conserved) {
                primitive_to_conserved_zone(primitive, conserved, i, j);
            }
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.ni + bs.x - 1) / bs.x, (mesh.nj + bs.y - 1) / bs.y); // WARNING: should x & y be transposed for non-square grids?
            primitive_to_conserved_kernel<<<bd, bs>>>(mesh, primitive, conserved);
            #endif
            break;
        }
    }
}


/**
 * Updates an array of primitive data by advancing it a single Runge-Kutta
 * step.
 * @param mesh                  The mesh [ni,     nj]
 * @param conserved_rk_ptr[in]  [ 0,  0] [ni,     nj]     [3]
 * @param primitive_rd_ptr[in]  [-2, -2] [ni + 4, nj + 4] [3]
 * @param primitive_wr_ptr[out] [-2, -2] [ni + 4, nj + 4] [3]
 * @param eos                   The EOS
 * @param buffer                The buffer region
 * @param masses[in]            A pointer a list of point mass objects
 * @param num_masses            The number of point masses
 * @param nu                    The viscosity coefficient
 * @param a                     The RK averaging parameter
 * @param dt                    The time step
 * @param mode                  The execution mode
 */
EXTERN_C void euler2d_advance_rk(
    struct Mesh mesh,
    real *conserved_rk_ptr,
    real *primitive_rd_ptr,
    real *primitive_wr_ptr,
    struct EquationOfState eos,
    struct BufferZone buffer,
    struct PointMass *masses,
    int num_masses,
    real nu,
    real a,
    real dt,
    real velocity_ceiling,
    real cooling_coefficient,
    real density_floor,
    real pressure_floor,
    enum ExecutionMode mode)
{
    struct Patch conserved_rk = patch(mesh, NCONS, 0, conserved_rk_ptr);
    struct Patch primitive_rd = patch(mesh, NCONS, 2, primitive_rd_ptr);
    struct Patch primitive_wr = patch(mesh, NCONS, 2, primitive_wr_ptr);

    switch (mode) {
        case CPU: {
            if (nu == 0.0) {
                FOR_EACH(conserved_rk) {
                    advance_rk_zone_inviscid(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
                }
            } else {
                FOR_EACH(conserved_rk) {
                    advance_rk_zone(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, nu, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
                }
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            if (nu == 0.0) {
                FOR_EACH_OMP(conserved_rk) {
                    advance_rk_zone_inviscid(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
                }
            } else {
                FOR_EACH_OMP(conserved_rk) {
                    advance_rk_zone(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, nu, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor, i, j);
                }
            }
            break;
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.ni + bs.x - 1) / bs.x, (mesh.nj + bs.y - 1) / bs.y);
            if (nu == 0.0) {
                advance_rk_kernel_inviscid<<<bd, bs>>>(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor);
            } else {
                advance_rk_kernel<<<bd, bs>>>(mesh, conserved_rk, primitive_rd, primitive_wr, eos, buffer, masses, num_masses, nu, a, dt, velocity_ceiling, cooling_coefficient, density_floor, pressure_floor);
            }
            #endif
            break;
        }
    }
}


/**
 * Fill a buffer with the maximum wavespeed in each zone.
 * @param  mesh               The mesh [ni,     nj]
 * @param  primitive_ptr[in]  [-2, -2] [ni + 4, nj + 4] [3]
 * @param  wavespeed_ptr[out] [ 0,  0] [ni,     nj]     [1]
 * @param eos                 The EOS
 * @param masses[in]          A pointer a list of point mass objects
 * @param num_masses          The number of point masses
 * @param mode                The execution mode
 */
EXTERN_C void euler2d_wavespeed(
    struct Mesh mesh,
    real *primitive_ptr,
    real *wavespeed_ptr,
    struct EquationOfState eos,
    struct PointMass *masses,
    int num_masses,
    enum ExecutionMode mode)
{
    struct Patch primitive = patch(mesh, NCONS, 2, primitive_ptr);
    struct Patch wavespeed = patch(mesh, 1,     0, wavespeed_ptr);

    switch (mode) {
        case CPU: {
            FOR_EACH(wavespeed) {
                wavespeed_zone(eos, primitive, wavespeed, i, j);
            }
            break;
        }

        case OMP: {
            #ifdef _OPENMP
            FOR_EACH_OMP(wavespeed) {
                wavespeed_zone(eos, primitive, wavespeed, i, j);
            }
            #endif
            break;
        }

        case GPU: {
            #if defined(__NVCC__) || defined(__ROCM__)
            dim3 bs = dim3(16, 16);
            dim3 bd = dim3((mesh.ni + bs.x - 1) / bs.x, (mesh.nj + bs.y - 1) / bs.y);
            wavespeed_kernel<<<bd, bs>>>(mesh, eos, primitive, wavespeed, masses, num_masses);
            #endif
            break;
        }
    }
}
