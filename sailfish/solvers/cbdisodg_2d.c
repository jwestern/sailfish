/*
MODULE: cbdisodg_2d

DESCRIPTION: Isothermal DG solver for a binary accretion problem in 2D planar
  cartesian coordinates.
*/


#define ORDER 3
#define NCONS 3
#define NPOLY 6


// ============================ MATH ==========================================
// ============================================================================
#define min2(a, b) ((a) < (b) ? (a) : (b))
#define max2(a, b) ((a) > (b) ? (a) : (b))
#define min3(a, b, c) min2(a, min2(b, c))
#define max3(a, b, c) max2(a, max2(b, c))
#define sign(x) copysign(1.0, x)
#define minabs(a, b, c) min3(fabs(a), fabs(b), fabs(c))


// ============================ INTERNAL STRUCTS ==============================
// ============================================================================
struct PointMass {
    double x;
    double y;
    double vx;
    double vy;
    double mass;
    double softening_length;
    double sink_rate;
    double sink_radius;
    int sink_model;
};

struct PointMassList {
    struct PointMass masses[2];
};

struct KeplerianBuffer {
    double surface_density;
    double surface_pressure;
    double central_mass;
    double driving_rate;
    double outer_radius;
    double onset_width;
    int is_enabled;
};


// ============================ GRAVITY =======================================
// ============================================================================
PRIVATE double gravitational_potential(
    struct PointMassList *mass_list,
    double x1,
    double y1)
{
    double phi = 0.0;

    for (int p = 0; p < 2; ++p)
    {
        if (mass_list->masses[p].mass > 0.0)
        {
            double x0 = mass_list->masses[p].x;
            double y0 = mass_list->masses[p].y;
            double mp = mass_list->masses[p].mass;
            double rs = mass_list->masses[p].softening_length;

            double dx = x1 - x0;
            double dy = y1 - y0;
            double r2 = dx * dx + dy * dy;
            double r2_softened = r2 + rs * rs;

            phi -= mp / sqrt(r2_softened);
        }
    }
    return phi;
}

PRIVATE void point_mass_source_term(
    struct PointMass *mass,
    double x1,
    double y1,
    double dt,
    double *prim,
    double *delta_cons)
{
    double x0 = mass->x;
    double y0 = mass->y;
    double sigma = prim[0];
    double dx = x1 - x0;
    double dy = y1 - y0;
    double r2 = dx * dx + dy * dy;
    double dr = sqrt(r2);
    double r_sink = mass->sink_radius;
    double r_soft = mass->softening_length;

    double fgrav_numerator = sigma * mass->mass * pow(r2 + r_soft * r_soft, -1.5);
    double fx = -fgrav_numerator * dx;
    double fy = -fgrav_numerator * dy;
    double sink_rate = (dr < 4.0 * r_sink) ? mass->sink_rate * exp(-pow(dr / r_sink, 4.0)) : 0.0;
    double mdot = sigma * sink_rate * -1.0;

    switch (mass->sink_model)
    {
        case 1: // acceleration-free
        {
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * mdot * prim[1] + dt * fx;
            delta_cons[2] = dt * mdot * prim[2] + dt * fy;
            break;
        }
        case 2: // torque-free
        {
            double vx = prim[1];
            double vy = prim[2];
            double vx0 = mass->vx;
            double vy0 = mass->vy;
            double rhatx = dx / (dr + 1e-12);
            double rhaty = dy / (dr + 1e-12);
            double dvdotrhat = (vx - vx0) * rhatx + (vy - vy0) * rhaty;
            double vxstar = dvdotrhat * rhatx + vx0;
            double vystar = dvdotrhat * rhaty + vy0;
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * mdot * vxstar + dt * fx;
            delta_cons[2] = dt * mdot * vystar + dt * fy;
            break;
        }
        case 3: // force-free
        {
            delta_cons[0] = dt * mdot;
            delta_cons[1] = dt * fx;
            delta_cons[2] = dt * fy;
            break;
        }
        default: // sink is inactive
        {
            delta_cons[0] = 0.0;
            delta_cons[1] = 0.0;
            delta_cons[2] = 0.0;
            break;
        }
    }
}

PRIVATE void point_masses_source_term(
    struct PointMassList *mass_list,
    double x1,
    double y1,
    double dt,
    double *prim,
    double *cons)
{
    for (int p = 0; p < 2; ++p)
    {
        double delta_cons[NCONS];
        point_mass_source_term(&mass_list->masses[p], x1, y1, dt, prim, delta_cons);

        for (int q = 0; q < NCONS; ++q)
        {
            cons[q] += delta_cons[q];
        }
    }
}

// ============================ EOS AND BUFFER ================================
// ============================================================================
PRIVATE double sound_speed_squared(
    double cs2,
    double mach_squared,
    int eos_type,
    double x,
    double y,
    struct PointMassList *mass_list)
{
    switch (eos_type)
    {
        case 1: // globally isothermal
            return cs2;
        case 2: // locally Isothermal
            return -gravitational_potential(mass_list, x, y) / mach_squared;
        default:
            return 1.0; // WARNING
    }
}

PRIVATE void buffer_source_term(
    struct KeplerianBuffer *buffer,
    double xc,
    double yc,
    double *cons,
    double *cons_dot)
{
    if (buffer->is_enabled)
    {
        double rc = sqrt(xc * xc + yc * yc);
        double surface_density = buffer->surface_density;
        double central_mass = buffer->central_mass;
        double driving_rate = buffer->driving_rate;
        double outer_radius = buffer->outer_radius;
        double onset_width = buffer->onset_width;
        double onset_radius = outer_radius - onset_width;

        if (rc > onset_radius)
        {
            double pf = surface_density * sqrt(central_mass / rc);
            double px = pf * (-yc / rc);
            double py = pf * ( xc / rc);
            double u0[NCONS] = {surface_density, px, py};

            double omega_outer = sqrt(central_mass * pow(onset_radius, -3.0));
            // double buffer_rate = driving_rate * omega_outer * max2(rc, 1.0);
            double buffer_rate = driving_rate * omega_outer * (rc - onset_radius) / (outer_radius - onset_radius);

            for (int q = 0; q < NCONS; ++q)
            {
                cons_dot[q] -= (cons[q] - u0[q]) * buffer_rate;
            }
        }
    }
}

// ============================ HYDRO =========================================
// ============================================================================
PRIVATE void conserved_to_primitive(
    const double *cons,
    double *prim,
    double velocity_ceiling)
{
    double rho = cons[0];
    double px = cons[1];
    double py = cons[2];
    double vx = sign(px) * min2(fabs(px / rho), velocity_ceiling);
    double vy = sign(py) * min2(fabs(py / rho), velocity_ceiling);

    prim[0] = rho;
    prim[1] = vx;
    prim[2] = vy;
}

PRIVATE double primitive_to_velocity(
    const double *prim,
    int direction)
{
    switch (direction)
    {
        case 0: return prim[1];
        case 1: return prim[2];
        default: return 0.0;
    }
}

PRIVATE void primitive_to_flux(
    const double *prim,
    const double *cons,
    double *flux,
    double cs2,
    int direction)
{
    double vn = primitive_to_velocity(prim, direction);
    double rho = prim[0];
    double pressure = rho * cs2;

    flux[0] = vn * cons[0];
    flux[1] = vn * cons[1] + pressure * (direction == 0);
    flux[2] = vn * cons[2] + pressure * (direction == 1);
}

PRIVATE void primitive_to_outer_wavespeeds(
    const double *prim,
    double *wavespeeds,
    double cs2,
    int direction)
{
    double cs = sqrt(cs2);
    double vn = primitive_to_velocity(prim, direction);
    wavespeeds[0] = vn - cs;
    wavespeeds[1] = vn + cs;
}

PRIVATE double primitive_max_wavespeed(
    const double *prim,
    double cs2)
{
    double cs = sqrt(cs2);
    double vx = prim[1];
    double vy = prim[2];
    double ax = max2(fabs(vx - cs), fabs(vx + cs));
    double ay = max2(fabs(vy - cs), fabs(vy + cs));
    return max2(ax, ay);
}

PRIVATE void riemann_hlle(
    const double *ul,
    const double *ur,
    double *flux,
    double cs2,
    double velocity_ceiling,
    int direction)
{
    double pl[NCONS];
    double pr[NCONS];
    double fl[NCONS];
    double fr[NCONS];
    double al[2];
    double ar[2];

    conserved_to_primitive(ul, pl, velocity_ceiling);
    conserved_to_primitive(ur, pr, velocity_ceiling);
    primitive_to_flux(pl, ul, fl, cs2, direction);
    primitive_to_flux(pr, ur, fr, cs2, direction);
    primitive_to_outer_wavespeeds(pl, al, cs2, direction);
    primitive_to_outer_wavespeeds(pr, ar, cs2, direction);

    const double am = min3(0.0, al[0], ar[0]);
    const double ap = max3(0.0, al[1], ar[1]);

    for (int q = 0; q < NCONS; ++q)
    {
        flux[q] = (fl[q] * ap - fr[q] * am - (ul[q] - ur[q]) * ap * am) / (ap - am);
    }
}

PRIVATE double basis_phi_face(
    int A_face,
    int j_poly,
    int i_quad)
{
    // Scaled LeGendre polynomials at internal quadrature points
    double phi_vol[3][3] = {{ 1.000000000000000, 1.000000000000000, 1.000000000000000},
                            {-1.341640786499873, 0.000000000000000, 1.341640786499873},
                            { 0.894427190999914, -1.11803398874990, 0.894427190999914}};

    // Scaled LeGendgre polynomials at the interval endpoints
    double phi_lface[3] = {1.000000000000000, -1.732050807568877, 2.23606797749979};
    double phi_rface[3] = {1.000000000000000, +1.732050807568877, 2.23606797749979};

    switch (A_face)
    {
        case 0: case 2: return phi_lface[j_poly] * phi_vol[j_poly][i_quad];
        case 1: case 3: return phi_rface[j_poly] * phi_vol[j_poly][i_quad];
    }
}

PRIVATE void reconstruct_cons_at_face(
    int A_face,
    int i_quad,
    double *weights,
    double *cons)
{
    // Gaussian weights at quadrature points
    double gauss_weights_1d[3] = { 0.555555555555556, 0.888888888888889, 0.555555555555556};

    for (int q_cons = 0; q_cons < NCONS; ++q_cons)
    {
        // 1st order for testing
        int j_poly = 0;
        cons[q_cons] = weights[q_cons * NPOLY + j_poly] * gauss_weights_1d[i_quad];

        // cons[q_cons] = 0.0;
        // for (int j_poly = 0; j_poly < NPOLY; ++j_poly)
        // {
        //     cons[q_cons] += weights[q_cons * NPOLY + j_poly] * gauss_weights_1d[i_quad];
        // }
    }
}


// ============================ PUBLIC API ====================================
// ============================================================================
PUBLIC void cbdisodg_2d_advance_rk_safe(
    int ni,
    int nj,
    double patch_xl, // mesh
    double patch_xr,
    double patch_yl,
    double patch_yr,
    double *weights0, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double *weights1, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double *weights2, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double buffer_surface_density,
    double buffer_central_mass,
    double buffer_driving_rate,
    double buffer_outer_radius,
    double buffer_onset_width,
    int buffer_is_enabled,
    double x1, // point mass 1
    double y1,
    double vx1,
    double vy1,
    double mass1,
    double softening_length1,
    double sink_rate1,
    double sink_radius1,
    int sink_model1,
    double x2, // point mass 2
    double y2,
    double vx2,
    double vy2,
    double mass2,
    double softening_length2,
    double sink_rate2,
    double sink_radius2,
    int sink_model2,
    double cs2, // equation of state
    double mach_squared,
    int eos_type,
    double nu, // kinematic viscosity coefficient
    double rk_param, // RK parameter
    double dt, // timestep
    double velocity_ceiling)
{
    // Gaussian quadrature points in scaled domain xsi=[-1,1]
    double xsi_quad[3] = {-0.774596669241483, 0.000000000000000, 0.774596669241483};

    // Gaussian weights at quadrature points
    double gauss_weights_1d[3] = {0.555555555555556, 0.888888888888889, 0.555555555555556};

    // Scaled LeGendre polynomials at internal quadrature points
    double phi_vol[3][3] = {{ 1.000000000000000, +1.00000000000000, +1.000000000000000},
                            {-1.341640786499873, +0.00000000000000, +1.341640786499873},
                            {+0.894427190999914, -1.11803398874990, +0.894427190999914}};

    // Unit normal vector at left and right faces
    double nhat[2] = {-1.0, 1.0};

    double dx = (patch_xr - patch_xl) / ni;
    double dy = (patch_yr - patch_yl) / nj;
    double face_area_vector[4] = {-dy, dy, -dx, dx}; // n-hat times the face area
    double cell_volume = dx * dy;

    int ng = 1; // number of guard zones
    int si = NCONS * NPOLY * (nj + 2 * ng);
    int sj = NCONS * NPOLY;

    FOR_EACH_2D(ni, nj)
    {
        double xl = patch_xl + (i + 0.0) * dx;
        double xc = patch_xl + (i + 0.5) * dx;
        double xr = patch_xl + (i + 1.0) * dx;
        double yl = patch_yl + (j + 0.0) * dy;
        double yc = patch_yl + (j + 0.5) * dy;
        double yr = patch_yl + (j + 1.0) * dy;

        int ncc = (i     + ng) * si + (j     + ng) * sj;
        int nli = (i - 1 + ng) * si + (j     + ng) * sj;
        int nri = (i + 1 + ng) * si + (j     + ng) * sj;
        int nlj = (i     + ng) * si + (j - 1 + ng) * sj;
        int nrj = (i     + ng) * si + (j + 1 + ng) * sj;

        double *ucc = &weights1[ncc];
        double *uli = &weights1[nli];
        double *uri = &weights1[nri];
        double *ulj = &weights1[nlj];
        double *urj = &weights1[nrj];

        int face_axis[4] = {0, 0, 1, 1};
        double equation_20[NCONS][NPOLY] = {0.0};
        double godunov_flux[4][NCONS]; // xl, xr, yl, yr
        double cs2 = 1.0;
        double up[NCONS];
        double um[NCONS];

        for (int i_quad; i_quad < ORDER; ++i_quad)
        {
            for (int A_face = 0; A_face < 4; ++A_face)
            {
                int opposing[4] = {1, 0, 3, 2};
                int B_face = opposing[A_face];
                reconstruct_cons_at_face(A_face, i_quad, ucc, up);
                reconstruct_cons_at_face(B_face, i_quad, uli, um);
                riemann_hlle(um, up, godunov_flux[A_face], cs2, velocity_ceiling, face_axis[A_face]);                
            }
        }

        for (int j_poly = 0; j_poly < NPOLY; ++j_poly)
        {
            for (int A_face = 0; A_face < 4; ++A_face)
            {
                for (int i_quad; i_quad < ORDER; ++i_quad)
                {
                    for (int q_cons = 0; q_cons < NCONS; ++q_cons)
                    {
                        equation_20[q_cons][j_poly] += (0.5 * face_area[A_face]
                            * godunov_flux[A_face][q_cons]
                            * basis_phi_face(A_face, j_poly, i_quad)
                            * gauss_weights_1d[i_quad]
                        );
                    }
                }
            }
        }

        double *w1 = &weights1[ncc];
        double *w2 = &weights2[ncc];

        for (int q_cons = 0; q_cons < NCONS; ++q_cons)
        {
            for (int j_poly = 0; j_poly < NPOLY; ++j_poly)
            {
                int n = q_cons * NPOLY + j_poly;
                w2[n] = w1[n] - 0.5 * equation_20[n] * dt / cell_volume;
            }
        }
    }
}


PUBLIC void cbdisodg_2d_advance_rk(
    int ni,
    int nj,
    double patch_xl, // mesh
    double patch_xr,
    double patch_yl,
    double patch_yr,
    double *weights0, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double *weights1, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double *weights2, // :: $.shape == (ni + 2, nj + 2, 3, 6) # 3, 6 = NCONS, NPOLY
    double buffer_surface_density,
    double buffer_central_mass,
    double buffer_driving_rate,
    double buffer_outer_radius,
    double buffer_onset_width,
    int buffer_is_enabled,
    double x1, // point mass 1
    double y1,
    double vx1,
    double vy1,
    double mass1,
    double softening_length1,
    double sink_rate1,
    double sink_radius1,
    int sink_model1,
    double x2, // point mass 2
    double y2,
    double vx2,
    double vy2,
    double mass2,
    double softening_length2,
    double sink_rate2,
    double sink_radius2,
    int sink_model2,
    double cs2, // equation of state
    double mach_squared,
    int eos_type,
    double nu, // kinematic viscosity coefficient
    double rk_param, // RK parameter
    double dt, // timestep
    double velocity_ceiling)
{
    // Gaussian quadrature points in scaled domain xsi=[-1,1]
    double g[3] = {-0.774596669241483, 0.000000000000000, 0.774596669241483};
    // Gaussian weights at quadrature points
    double w[3] = { 0.555555555555556, 0.888888888888889, 0.555555555555556};
    // Scaled LeGendre polynomials at 1D quadrature points
    double p[3][3] = {{ 1.000000000000000, 1.000000000000000, 1.000000000000000},
                      {-1.341640786499873, 0.000000000000000, 1.341640786499873},
                      { 0.894427190999914, -1.11803398874990, 0.894427190999914}};
    // Derivative of Scaled LeGendre polynomials at 1D quadrature points
    double pp[3][3] = {{ 0.000000000000000, 0.000000000000000, 0.000000000000000},
                       { 1.732050807568877, 1.732050807568877, 1.732050807568877},
                       {-5.196152422706629, 0.000000000000000, 5.196152422706629}};
    // Scaled LeGendre polynomials at left face
    double pfl[3] = {1.000000000000000, -1.732050807568877, 2.23606797749979};
    // Derivative of Scaled LeGendre polynomials at left face
    double ppfl[3] = {0.000000000000000, 1.732050807568877, -6.708203932499369};
    // Scaled LeGendre polynomials at right face
    double pfr[3] = {1.000000000000000,  1.732050807568877, 2.23606797749979};
    // Derivative of Scaled LeGendre polynomials at right face
    double ppfr[3] = {0.000000000000000, 1.732050807568877, 6.708203932499369};

    // Unit normal vector at left and right faces
    double nhat[2] = {-1.0, 1.0};

    struct KeplerianBuffer buffer = {
        buffer_surface_density,
        buffer_central_mass,
        buffer_driving_rate,
        buffer_outer_radius,
        buffer_onset_width,
        buffer_is_enabled
    };
    struct PointMass m1 = {x1, y1, vx1, vy1, mass1, softening_length1, sink_rate1, sink_radius1, sink_model1};
    struct PointMass m2 = {x2, y2, vx2, vy2, mass2, softening_length2, sink_rate2, sink_radius2, sink_model2};
    struct PointMassList mass_list = {{m1, m2}};

    double dx = (patch_xr - patch_xl) / ni;
    double dy = (patch_yr - patch_yl) / nj;

    int ng = 1; // number of guard zones
    int si = NCONS * NPOLY * (nj + 2 * ng);
    int sj = NCONS * NPOLY;

    FOR_EACH_2D(ni, nj)
    {
        double xl = patch_xl + (i + 0.0) * dx;
        double xc = patch_xl + (i + 0.5) * dx;
        double xr = patch_xl + (i + 1.0) * dx;
        double yl = patch_yl + (j + 0.0) * dy;
        double yc = patch_yl + (j + 0.5) * dy;
        double yr = patch_yl + (j + 1.0) * dy;

        // ------------------------------------------------------------------------
        //
        //
        //      +-------+-------+-------+
        //      |       |       | x x x |   x(ic, jc) = quadrature points in each zone
        //      |       |  rj   | x x x |
        //      |       |       | x x x |
        //      +-------+-------+-------+
        //      |       |       |       |
        //      |  li  -|+  c  -|+  ri  |
        //      |       |       |       |
        //      +-------+-------+-------+
        //      |       |       |       |
        //      |       |  lj   |       |
        //      |       |       |       |
        //      +-------+-------+-------+
        //
        //
        // ------------------------------------------------------------------------

        int ncc = (i     + ng) * si + (j     + ng) * sj;
        int nli = (i - 1 + ng) * si + (j     + ng) * sj;
        int nri = (i + 1 + ng) * si + (j     + ng) * sj;
        int nlj = (i     + ng) * si + (j - 1 + ng) * sj;
        int nrj = (i     + ng) * si + (j + 1 + ng) * sj;

        double *ucc = &weights1[ncc];
        double *uli = &weights1[nli];
        double *uri = &weights1[nri];
        double *ulj = &weights1[nlj];
        double *urj = &weights1[nrj];

        double flux[NCONS];
        double um[NCONS];
        double up[NCONS];
        double dudxm[NCONS];
        double dudxp[NCONS];
        double dudym[NCONS];
        double dudyp[NCONS];

        // interior node values of basis function phi and derivatives
        double phi[NPOLY];
        double dphidx[NPOLY];
        double dphidy[NPOLY];

        // left face node values of basis function phi and derivatives
        double phil[NPOLY];
        double dphidxl[NPOLY];
        double dphidyl[NPOLY];

        // right face node values of basis function phi and derivatives
        double phir[NPOLY];
        double dphidxr[NPOLY];
        double dphidyr[NPOLY];

        double surface_term[NCONS * NPOLY];
        double volume_term[NCONS * NPOLY];

        for (int q = 0; q < NCONS; ++q)
        {
            for (int l = 0; l < NPOLY; ++l)
            {
                surface_term[NPOLY * q + l] = 0.0;
                volume_term[ NPOLY * q + l] = 0.0;
            }
        }

        // Volume term including source terms
        for (int ic = 0; ic < 3; ++ic)
        {
            for (int jc = 0; jc < 3; ++jc)
            {
                double xp = xc + 0.5 * g[ic] * dx;
                double yp = yc + 0.5 * g[jc] * dy;

                double cs2p = sound_speed_squared(cs2, mach_squared, eos_type, xp, yp, &mass_list);

                // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) and derivatives at cell points
                int il = 0;
                for (int m = 0; m < 3; ++m)
                {
                    for (int n = 0; n < 3; ++n)
                    {
                        if ((n + m) < 3)
                        {
                            phi[il]  =  p[m][ic] *  p[n][jc];
                            dphidx[il] = pp[m][ic] *  p[n][jc];
                            dphidy[il] =  p[m][ic] * pp[n][jc];
                            il += 1;
                        }
                    }
                }

                double uij[NCONS];
                double pij[NCONS];
                double dudx[NCONS];
                double dudy[NCONS];

                for (int q = 0; q < NCONS; ++q)
                {
                    uij[q] = 0.0;
                    dudx[q] = 0.0;
                    dudy[q] = 0.0;

                    for (int l = 0; l < NPOLY; ++l)
                    {
                        uij[q]  += ucc[NPOLY * q + l] * phi[l];
                        dudx[q] += ucc[NPOLY * q + l] * dphidx[l];
                        dudy[q] += ucc[NPOLY * q + l] * dphidy[l];
                    }
                }

                double cons_dot[NCONS];

                for (int q = 0; q < NCONS; ++q)
                {
                    cons_dot[q] = 0.0;
                }

                conserved_to_primitive(uij, pij, velocity_ceiling);
                
                //buffer_source_term(&buffer, xp, yp, uij, cons_dot);
                //point_masses_source_term(&mass_list, xp, yp, 1.0, pij, cons_dot);

                double flux_x[NCONS];
                double flux_y[NCONS];

                primitive_to_flux(pij, uij, flux_x, cs2p, 0);
                primitive_to_flux(pij, uij, flux_y, cs2p, 1);

                for (int q = 0; q < NCONS; ++q)
                {
                    for (int l = 0; l < NPOLY; ++l)
                    {
                        volume_term[NPOLY * q + l] +=
                            w[ic] * w[jc] *
                            (flux_x[q] * dphidx[l] * dx + flux_y[q] * dphidy[l] * dy);
                                //+ 0.5 * dx * dy * cons_dot[q] * phi[l]);
                    }
                }
            }
        }

        // Surface terms; loop over face nodes (one face at a time)
        
        // Left face
        for (int jp = 0; jp < 3; ++jp)
        {
            double yp = yc + 0.5 * g[jp] * dy;

            double cs2p = sound_speed_squared(cs2, mach_squared, eos_type, xl, yp, &mass_list);

            // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) 
            int il = 0;
            for (int m = 0; m < 3; ++m)
            {
                for (int n = 0; n < 3; ++n)
                {
                    if ((n + m) < 3)
                    {
                        // phi at left side of zone
                        phil[il]    =    pfl[m] *  p[n][jp];
                        // phi and at right side of zone
                        phir[il]    =    pfr[m] *  p[n][jp];      
                        il += 1;
                    }
                }
            }
            for (int q = 0; q < NCONS; ++q){

                // minus side of face
                um[q] = 0.0; 

                // plus side of face
                up[q] = 0.0; 

                for (int l = 0; l < NPOLY; ++l)
                {
                    // "minus side": right face of zone i-1
                    um[q]    += uli[NPOLY * q + l] * phir[l]; 

                    // "plus side": left face of zone i
                    up[q]    += ucc[NPOLY * q + l] * phil[l]; 
                }
            }

            riemann_hlle(um, up, flux, cs2p, velocity_ceiling, 0);

            for (int q = 0; q < NCONS; ++q)
            {
                for (int l = 0; l < NPOLY; ++l)
                {
                    surface_term[NPOLY * q + l] -= flux[q] * nhat[0] * phil[l] * w[jp] * dx;
                }
            }            
        }

        // Right face
        for (int jp = 0; jp < 3; ++jp)
        {
            double yp = yc + 0.5 * g[jp] * dy;
            double cs2p = sound_speed_squared(cs2, mach_squared, eos_type, xr, yp, &mass_list);
            int il = 0;

            // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) and derivatives at face nodes
            for (int m = 0; m < 3; ++m)
            {
                for (int n = 0; n < 3; ++n)
                {
                    if ((n + m) < 3)
                    {
                        // phi and derivatives at left side of zone
                        phil[il]    =    pfl[m] *  p[n][jp];
                        // phi and derivatives at right side of zone
                        phir[il]    =    pfr[m] *  p[n][jp];      
                        il += 1;
                    }
                }
            }

            for (int q = 0; q < NCONS; ++q)
            {
                // minus side of face
                um[q] = 0.0; 

                // plus side of face
                up[q] = 0.0; 

                for (int l = 0; l < NPOLY; ++l)
                {
                    // "minus side": right face of zone i
                    um[q]    += ucc[NPOLY * q + l] * phir[l]; 

                    // "plus side": left face of zone i+1
                    up[q]    += uri[NPOLY * q + l] * phil[l]; 
                }
            }

            riemann_hlle(um, up, flux, cs2p, velocity_ceiling, 0);

            for (int q = 0; q < NCONS; ++q)
            {
                for (int l = 0; l < NPOLY; ++l)
                {
                    surface_term[NPOLY * q + l] -= flux[q] * nhat[1] * phir[l] * w[jp] * dx;
                }
            }            
        }

        // Bottom face
        for (int ip = 0; ip < 3; ++ip)
        {
            double xp = xc + 0.5 * g[ip] * dx;

            double cs2p = sound_speed_squared(cs2, mach_squared, eos_type, xp, yl, &mass_list);

            // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) and derivatives at face nodes
            int il = 0;
            for (int m = 0; m < 3; ++m)
            {
                for (int n = 0; n < 3; ++n)
                {
                    if ((n + m) < 3)
                    {
                        // phi and derivatives at left side of zone
                        phil[il]    =    pfl[m] *  p[n][ip];
                        // phi and derivatives at right side of zone
                        phir[il]    =    pfr[m] *  p[n][ip];     
                        il += 1;
                    }
                }
            }

            for (int q = 0; q < NCONS; ++q){

                // minus side of face
                um[q] = 0.0; 

                // plus side of face
                up[q] = 0.0; 

                for (int l = 0; l < NPOLY; ++l)
                {
                    // "minus side": top face of zone j-1
                    um[q]    += ulj[NPOLY * q + l] * phir[l]; 

                    // "plus side": bottom face of zone ij
                    up[q]    += ucc[NPOLY * q + l] * phil[l]; 
                }
            }

            riemann_hlle(um, up, flux, cs2p, velocity_ceiling, 1);

            for (int q = 0; q < NCONS; ++q)
            {
                for (int l = 0; l < NPOLY; ++l)
                {
                    surface_term[NPOLY * q + l] -= flux[q] * nhat[0] * phil[l] * w[ip] * dy;
                }
            }            
        }

        // Top face
        for (int ip = 0; ip < 3; ++ip)
        {
            double xp = xc + 0.5 * g[ip] * dx;

            double cs2p = sound_speed_squared(cs2, mach_squared, eos_type, xp, yr, &mass_list);

            // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) and derivatives at face nodes
            int il = 0;
            for (int m = 0; m < 3; ++m)
            {
                for (int n = 0; n < 3; ++n)
                {
                    if ((n + m) < 3)
                    {
                        // phi and derivatives at left side of zone
                        phil[il]    =    pfl[m] *  p[n][ip];
                        // phi and derivatives at right side of zone
                        phir[il]    =    pfr[m] *  p[n][ip];      
                        il += 1;
                    }
                }
            }

            for (int q = 0; q < NCONS; ++q){

                // minus side of face
                um[q] = 0.0; 

                // plus side of face
                up[q] = 0.0; 

                for (int l = 0; l < NPOLY; ++l)
                {
                    // "minus side": top face of zone j
                    um[q]    += ucc[NPOLY * q + l] * phir[l]; 

                    // "plus side": bottom face of zone j+1
                    up[q]    += urj[NPOLY * q + l] * phil[l]; 
                }
            }

            riemann_hlle(um, up, flux, cs2p, velocity_ceiling, 1);

            for (int q = 0; q < NCONS; ++q)
            {
                for (int l = 0; l < NPOLY; ++l)
                {
                    surface_term[NPOLY * q + l] -= flux[q] * nhat[1] * phir[l] * w[ip] * dy;
                }
            }            
        }

        double *w0 = &weights0[ncc];
        double *w1 = &weights1[ncc];
        double *w2 = &weights2[ncc];

        for (int q = 0; q < NCONS; ++q)
        {
            for (int l = 0; l < 1; ++l)
            {
                int n = NPOLY * q + l;
                w2[n] = w1[n] + 0.5 * (surface_term[n] + volume_term[n]) * dt / (dx * dy);
                //w2[n] = w1[n] + 0.5 * (volume_term[n]) * dt / (dx * dy);
                //w2[n] = w1[n] + 0.5 * (surface_term[n]) * dt / (dx * dy);

                w2[n] = (1.0 - rk_param) * w2[n] + rk_param * w0[n];
            }
        }
    }
}

PUBLIC void cbdisodg_2d_point_mass_source_term(
    int ni,
    int nj,
    double patch_xl, // mesh
    double patch_xr,
    double patch_yl,
    double patch_yr,
    double x1, // point mass 1
    double y1,
    double vx1,
    double vy1,
    double mass1,
    double softening_length1,
    double sink_rate1,
    double sink_radius1,
    int sink_model1,
    double x2, // point mass 2
    double y2,
    double vx2,
    double vy2,
    double mass2,
    double softening_length2,
    double sink_rate2,
    double sink_radius2,
    int sink_model2,
    double velocity_ceiling,
    int which_mass, // :: $ in [1, 2]
    double *weights, // :: $.shape == (ni + 2, nj + 2, 3, 6)
    double *cons_rate) // :: $.shape == (ni + 2, nj + 2, 3)
{
    struct PointMass m1 = {x1, y1, vx1, vy1, mass1, softening_length1, sink_rate1, sink_radius1, sink_model1};
    struct PointMass m2 = {x2, y2, vx2, vy2, mass2, softening_length2, sink_rate2, sink_radius2, sink_model2};
    struct PointMassList mass_list = {{m1, m2}};

    // Gaussian quadrature points in scaled domain xsi=[-1,1]
    double g[3] = {-0.774596669241483, 0.000000000000000, 0.774596669241483};
    // Gaussian weights at quadrature points
    double w[3] = { 0.555555555555556, 0.888888888888889, 0.555555555555556};
        // Scaled LeGendre polynomials at quadrature points
    double p[3][3] = {{ 1.000000000000000, 1.000000000000000, 1.000000000000000},
                      {-1.341640786499873, 0.000000000000000, 1.341640786499873},
                      { 0.894427190999914, -1.11803398874990, 0.894427190999914}};

    int ng = 1; // number of guard zones
    int si = NCONS * NPOLY * (nj + 2 * ng);
    int sj = NCONS * NPOLY;

    double dx = (patch_xr - patch_xl) / ni;
    double dy = (patch_yr - patch_yl) / nj;

    FOR_EACH_2D(ni, nj)
    {
        int ncc = (i + ng) * si + (j + ng) * sj;
        double *ucc = &weights[ncc];
        double *udot = &cons_rate[ncc];        
        double xc = patch_xl + (i + 0.5) * dx;
        double yc = patch_yl + (j + 0.5) * dy;
        // double *pc = &primitive[ncc];
        // double *uc = &cons_rate[ncc];
        // point_mass_source_term(&mass_list.masses[which_mass - 1], xc, yc, 1.0, pc, uc);

        double u_dot[NCONS];
        double u_dot_sum[NCONS];
        double phi[NPOLY];

        for (int q = 0; q < NCONS; ++q)
        {
            u_dot[q]     = 0.0;
            u_dot_sum[q] = 0.0;
        }

        for (int ic = 0; ic < 3; ++ic)
        {
            for (int jc = 0; jc < 3; ++jc)
            {
                double xp = xc + 0.5 * g[ic] * dx;
                double yp = yc + 0.5 * g[jc] * dy;
                
                // 2D basis functions phi_l(x,y) = P_m(x) * P_n(y) at cell points
                int il = 0;
                for (int m = 0; m < 3; ++m)
                {
                    for (int n = 0; n < 3; ++n)
                    {
                        if ((n + m) < 3)
                        {
                            phi[il]  =  p[m][ic] *  p[n][jc];
                            il += 1;
                        }
                    }
                }

                double uij[NCONS];
                double pij[NCONS];

                for (int q = 0; q < NCONS; ++q)
                {
                    uij[q] = 0.0;

                    for (int l = 0; l < NPOLY; ++l)
                    {
                        uij[q] += ucc[NPOLY * q + l] * phi[l];
                    }
                }

                conserved_to_primitive(uij, pij, velocity_ceiling);
                point_mass_source_term(&mass_list.masses[which_mass - 1], xp, yp, 1.0, pij, u_dot);
                for (int q = 0; q < NCONS; ++q)
                {
                    u_dot_sum[q] += w[ic] * w[jc] * u_dot[q];
                }
            }
        }

        for (int q = 0; q < NCONS; ++q)
        {
            udot[q] = u_dot_sum[q];
        }
    }
}


PUBLIC void cbdisodg_2d_wavespeed(
    int ni, // mesh
    int nj,
    double patch_xl,
    double patch_xr,
    double patch_yl,
    double patch_yr,
    double soundspeed2, // equation of state
    double mach_squared,
    int eos_type,
    double x1, // point mass 1
    double y1,
    double vx1,
    double vy1,
    double mass1,
    double softening_length1,
    double sink_rate1,
    double sink_radius1,
    int sink_model1,
    double x2, // point mass 2
    double y2,
    double vx2,
    double vy2,
    double mass2,
    double softening_length2,
    double sink_rate2,
    double sink_radius2,
    int sink_model2,
    double velocity_ceiling,
    double *weights,   // :: $.shape == (ni + 2, nj + 2, 3, 6)
    double *wavespeed) // :: $.shape == (ni + 2, nj + 2)
{
    struct PointMass m1 = {x1, y1, vx1, vy1, mass1, softening_length1, sink_rate1, sink_radius1, sink_model1};
    struct PointMass m2 = {x2, y2, vx2, vy2, mass2, softening_length2, sink_rate2, sink_radius2, sink_model2};
    struct PointMassList mass_list = {{m1, m2}};

    int ng = 1; // number of guard zones
    int si = NCONS * NPOLY * (nj + 2 * ng);
    int sj = NCONS * NPOLY;
    int ti = nj + 2 * ng;
    int tj = 1;
    double dx = (patch_xr - patch_xl)/ni;
    double dy = (patch_yr - patch_yl)/nj;

    FOR_EACH_2D(ni, nj)
    {
        int np = (i + ng) * si + (j + ng) * sj;
        int na = (i + ng) * ti + (j + ng) * tj;
        double x = patch_xl + (i + 0.5) * dx;
        double y = patch_yl + (j + 0.5) * dy;
        
        double *ucc = &weights[np];

        double uij[NCONS];
        double pij[NCONS];

        // use zeroth weights for zone average of conserved variables
        for (int q = 0; q < NCONS; ++q)
        {
            uij[q] = ucc[NPOLY * q + 0];
        }

        conserved_to_primitive(uij, pij, velocity_ceiling);
        double cs2 = sound_speed_squared(soundspeed2, mach_squared, eos_type, x, y, &mass_list);
        double a = primitive_max_wavespeed(pij, cs2);
        wavespeed[na] = a;
    }
}