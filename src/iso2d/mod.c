#include <math.h>
#include "../sailfish.h"


// ============================ COMPAT ========================================
// ============================================================================
#ifndef __NVCC__
#define __device__
#define __host__
#endif


// ============================ PHYSICS =======================================
// ============================================================================
#define NCONS 3
#define PLM_THETA 1.5


// ============================ MATH ==========================================
// ============================================================================
#define real double
#define min2(a, b) ((a) < (b) ? (a) : (b))
#define max2(a, b) ((a) > (b) ? (a) : (b))
#define min3(a, b, c) min2(a, min2(b, c))
#define max3(a, b, c) max2(a, max2(b, c))
#define sign(x) copysign(1.0, x)
#define minabs(a, b, c) min3(fabs(a), fabs(b), fabs(c))

static __device__ real plm_gradient_scalar(real yl, real y0, real yr)
{
    real a = (y0 - yl) * PLM_THETA;
    real b = (yr - yl) * 0.5;
    real c = (yr - y0) * PLM_THETA;
    return 0.25 * fabs(sign(a) + sign(b)) * (sign(a) + sign(c)) * minabs(a, b, c);
}

static __device__ void plm_gradient(real *yl, real *y0, real *yr, real *g)
{
    for (int q = 0; q < NCONS; ++q)
    {
        g[q] = plm_gradient_scalar(yl[q], y0[q], yr[q]);
    }
}


// ============================ GRAVITY =======================================
// ============================================================================
static __device__ real gravitational_potential(
    struct PointMass *masses,
    int num_masses,
    real x1,
    real y1)
{
    real phi = 0.0;

    for (int p = 0; p < num_masses; ++p)
    {
        real x0 = masses[p].x;
        real y0 = masses[p].y;
        real mp = masses[p].mass;
        real rs = masses[p].radius;

        real dx = x1 - x0;
        real dy = y1 - y0;
        real r2 = dx * dx + dy * dy;
        real r2_soft = r2 + rs * rs;

        phi -= mp / sqrt(r2_soft);
    }
    return phi;
}

static __device__ void point_mass_source_term(
    struct PointMass *mass,
    real x1,
    real y1,
    real dt,
    real sigma,
    real *delta_cons)
{
    real x0 = mass->x;
    real y0 = mass->y;
    real mp = mass->mass;
    real rs = mass->radius;

    real dx = x1 - x0;
    real dy = y1 - y0;
    real r2 = dx * dx + dy * dy;
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

    // NOTE: This is a force-free sink.
    delta_cons[0] = dt * sigma * sink_rate * -1.0;
    delta_cons[1] = dt * fx;
    delta_cons[2] = dt * fy;
}

static __device__ void point_masses_source_term(
    struct PointMass* masses,
    int num_masses,
    real x1,
    real y1,
    real dt,
    real sigma,
    real *cons)
{
    for (int p = 0; p < num_masses; ++p)
    {
        real delta_cons[NCONS];
        point_mass_source_term(&masses[p], x1, y1, dt, sigma, delta_cons);

        for (int q = 0; q < NCONS; ++q)
        {
            cons[q] += delta_cons[q];
        }
    }
}


// ============================ EOS AND BUFFER ================================
// ============================================================================
static __device__ real sound_speed_squared(
    struct EquationOfState *eos,
    real x,
    real y,
    struct PointMass *masses,
    int num_masses)
{
    switch (eos->type)
    {
        case Isothermal:
            return eos->isothermal.sound_speed_squared;
        case LocallyIsothermal:
            return -gravitational_potential(masses, num_masses, x, y) / eos->locally_isothermal.mach_number_squared;
        case GammaLaw:
            return 1.0; // WARNING
    }
    return 0.0;
}

static __device__ void buffer_source_term(
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
                real u0[NCONS] = {surface_density, px, py};

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

static __device__ void shear_strain(const real *gx, const real *gy, real dx, real dy, real *s)
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
static __device__ void conserved_to_primitive(const real *cons, real *prim)
{
    real rho = cons[0];
    real px = cons[1];
    real py = cons[2];
    real vx = px / rho;
    real vy = py / rho;

    prim[0] = rho;
    prim[1] = vx;
    prim[2] = vy;
}

static __device__ void primitive_to_conserved(const real *prim, real *cons)
{
    real rho = prim[0];
    real vx = prim[1];
    real vy = prim[2];
    real px = vx * rho;
    real py = vy * rho;

    cons[0] = rho;
    cons[1] = px;
    cons[2] = py;
}

static __device__ real primitive_to_velocity(const real *prim, int direction)
{
    switch (direction)
    {
        case 0: return prim[1];
        case 1: return prim[2];
        default: return 0.0;
    }
}

static __device__ void primitive_to_flux(
    const real *prim,
    const real *cons,
    real *flux,
    real cs2,
    int direction)
{
    real vn = primitive_to_velocity(prim, direction);
    real rho = prim[0];
    real pressure = rho * cs2;

    flux[0] = vn * cons[0];
    flux[1] = vn * cons[1] + pressure * (direction == 0);
    flux[2] = vn * cons[2] + pressure * (direction == 1);
}

static __device__ void primitive_to_outer_wavespeeds(
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

static __device__ real primitive_max_wavespeed(const real *prim, real cs2)
{
    real cs = sqrt(cs2);
    real vx = prim[1];
    real vy = prim[2];
    real ax = max2(fabs(vx - cs), fabs(vx + cs));
    real ay = max2(fabs(vy - cs), fabs(vy + cs));
    return max2(ax, ay);
}

static __device__ void riemann_hlle(const real *pl, const real *pr, real *flux, real cs2, int direction)
{
    real ul[NCONS];
    real ur[NCONS];
    real fl[NCONS];
    real fr[NCONS];
    real al[2];
    real ar[2];

    primitive_to_conserved(pl, ul);
    primitive_to_conserved(pr, ur);
    primitive_to_flux(pl, ul, fl, cs2, direction);
    primitive_to_flux(pr, ur, fr, cs2, direction);
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
    int num_fields;
    int start[2];
    int count[2];
    int jumps[2];
    real *data;
};

static struct Patch patch(struct Mesh mesh, int num_guard, real *data)
{
    struct Patch patch;
    patch.num_fields = NCONS;
    patch.start[0] = -num_guard;
    patch.start[1] = -num_guard;
    patch.count[0] = mesh.ni + 2 * num_guard;
    patch.count[1] = mesh.nj + 2 * num_guard;
    patch.jumps[0] = patch.num_fields * patch.count[1];
    patch.jumps[1] = patch.num_fields;
    patch.data = data;
    return patch;
}

// ============================ PUBLIC API ====================================
// ============================================================================

/**
 * Converts an array of primitive data to an array of conserved data. The
 * array index space must follow the descriptions below.
 * @param mesh          The mesh (ni, nj)
 * @param primitive_ptr Array of primitive data: start(-2, -2) count(ni + 4, nj + 4)
 * @param conserved_ptr Array of conserved data: start(0, 0) count(ni, nj)
 * @param mode          The execution mode
 */
void iso2d_primitive_to_conserved(struct Mesh mesh, real *primitive_ptr, real *conserved_ptr, enum ExecutionMode mode)
{
    struct Patch primitive = patch(mesh, 2, primitive_ptr);
    struct Patch conserved = patch(mesh, 0, conserved_ptr);    
    FOR_EACH(conserved) {
        real *u = GET(conserved, i, j);
        real *p = GET(primitive, i, j);
        primitive_to_conserved(p, u);
    }
}
