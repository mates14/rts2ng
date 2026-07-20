/*
 *  norad.h v. 01.beta 03/17/2001
 *
 *  Header file for norad.c
 */

// base note: header ported verbatim for the tle_t type (a plain data
// struct, used unconditionally as a Telescope member) and the SGP4/SDP4/etc
// declarations. The actual NORAD satellite propagation implementation
// (lib/pluto/*.cpp in the classic tree, ~3100 lines: sgp/sgp4/sgp8/sdp4/
// sdp8/deep/common/basics/get_el/dynamic/satellit/tle_out) is NOT ported -
// satellite (TLE) tracking is a genuinely optional, specialized feature by
// the same driver-tier reasoning used for SEP in camd and SimbadTarget in
// the monitor. Telescope::calculateTLE()/moveTLE()/parseTLE() are stubbed
// in teld.cpp to log an error instead of calling these functions, so
// nothing here ever actually gets called or needs linking - see the note
// there. lat_alt_to_parallax()/observer_cartesian_coords()/
// get_satellite_ra_dec_delta() (pluto/observe.h) are a different, small,
// self-contained file that *is* ported for real, since Telescope::
// initValues() calls lat_alt_to_parallax() unconditionally.

#ifndef NORAD_H
#define NORAD_H 1

/* Two-line-element satellite orbital data */
typedef struct
{
  double epoch, xndt2o, xndd6o, bstar;
  double xincl, xnodeo, eo, omegao, xmo, xno;
  int norad_number, bulletin_number, revolution_number;
  char classification;    /* "U" = unclassified;  only type I've seen */
  char ephemeris_type;
  char intl_desig[9];
} tle_t;

   /* NOTE: xndt2o and xndt6o are used only in the "classic" SGP, */
   /* not in SxP4 or SxP8. */
   /* xmo = mean anomaly at epoch */
   /* xno = mean motion at epoch */

#define DEEP_ARG_T_PARAMS     94

#define N_SGP_PARAMS          11
#define N_SGP4_PARAMS         30
#define N_SGP8_PARAMS         25
#define N_SDP4_PARAMS        (10 + DEEP_ARG_T_PARAMS)
#define N_SDP8_PARAMS        (11 + DEEP_ARG_T_PARAMS)

/* 94 = maximum possible size of the 'deep_arg_t' structure,  in 8-byte units */
/* You can use the above constants to minimize the amount of memory used,
   but if you use the following constant,  you can be assured of having
   enough memory for any of the five models: */

#define N_SAT_PARAMS         (11 + DEEP_ARG_T_PARAMS)

/* Byte 63 of the first line of a TLE contains the ephemeris type.  The */
/* following five values are recommended,  but it seems the non-zero    */
/* values are only used internally;  "published" TLEs all have type 0.  */

#define TLE_EPHEMERIS_TYPE_DEFAULT           0
#define TLE_EPHEMERIS_TYPE_SGP               1
#define TLE_EPHEMERIS_TYPE_SGP4              2
#define TLE_EPHEMERIS_TYPE_SDP4              3
#define TLE_EPHEMERIS_TYPE_SGP8              4
#define TLE_EPHEMERIS_TYPE_SDP8              5

/* SDP4 and SGP4 can return zero,  or any of the following error/warning codes.
The 'warnings' result in a mathematically reasonable value being returned,
and perigee within the earth is completely reasonable for an object that's
just left the earth or is about to hit it.  The 'errors' mean that no
reasonable position/velocity was determined.       */

#define SXPX_ERR_NEARLY_PARABOLIC         -1
#define SXPX_ERR_NEGATIVE_MAJOR_AXIS      -2
#define SXPX_WARN_ORBIT_WITHIN_EARTH      -3
#define SXPX_WARN_PERIGEE_WITHIN_EARTH    -4
#define SXPX_ERR_NEGATIVE_XN              -5

#ifdef __cplusplus
extern "C" {
#endif

void SGP_init (double *params, const tle_t *tle);
int  SGP ( const double tsince, const tle_t *tle, const double *params,
                                     double *pos, double *vel);

void SGP4_init (double *params, const tle_t *tle);
int  SGP4 ( const double tsince, const tle_t *tle, const double *params,
                                     double *pos, double *vel);

void SGP8_init (double *params, const tle_t *tle);
int  SGP8 ( const double tsince, const tle_t *tle, const double *params,
                                     double *pos, double *vel);

void SDP4_init (double *params, const tle_t *tle);
int  SDP4 ( const double tsince, const tle_t *tle, const double *params,
                                     double *pos, double *vel);

void SDP8_init (double *params, const tle_t *tle);
int  SDP8 ( const double tsince, const tle_t *tle, const double *params,
                                     double *pos, double *vel);

int select_ephemeris ( const tle_t *tle);
int parse_elements ( const char *line1, const char *line2, tle_t *sat);

#ifdef __cplusplus
}                       /* end of 'extern "C"' section */
#endif

#endif
