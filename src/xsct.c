/*
** xsct.c
** X11 set color temperature
** Public domain
*/

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/extensions/Xrandr.h>

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#define XSCT_VERSION              "2.4"
#define XSCT_PREFIX               "XSCT_"
#define XSCT_TEMPERATURE_DAY      XSCT_PREFIX "TEMPERATURE_DAY"
#define XSCT_TEMPERATURE_NIGHT    XSCT_PREFIX "TEMPERATURE_NIGHT"

#define BRIGHTNESS_DIV    65470.988

#define MINTEMP       700
#define TEMP_NORM     6500

#define MIN_DELTA     -1000000

#define GAMMA_MULT    65535.0
/*
** Approximation of the `redshift` table without limits.
** GAMMA = K0 + K1 * ln(T - T0)
** red range (T0 = MINTEMP)
** green color
*/
#define GAMMA_K0GR    -1.47751309139817
#define GAMMA_K1GR    0.28590164772055
/* blue color */
#define GAMMA_K0BR    -4.38321650114872
#define GAMMA_K1BR    0.6212158769447
/* blue range  (T0 = TEMP_NORM - MINTEMP) */
/* red color */
#define GAMMA_K0RB    1.75390204039018
#define GAMMA_K1RB    -0.1150805671482
/* green color */
#define GAMMA_K0GB    1.49221604915144
#define GAMMA_K1GB    -0.07513509588921


#if !defined(MAX)
#define MAX(x, y)    (((x) > (y)) ? (x) : (y))
#endif


typedef struct tempstate {
  long temp;
  double brightness;
} tempstate;


typedef struct sgamma {
  double red;
  double green;
  double blue;
} sgamma;


static const char *progname = NULL;   /* program name */
static int fail = 0;                  /* error flag */
static int crtc_arg = -1;             /* crtc index */
static int screen_arg = -1;           /* screen index */
static int verbose = 0;               /* do not by debug gamma by default */
static long temp_day = TEMP_NORM;     /* default "day" temperature */
static long temp_night = 4500;        /* default "night" temperature */


/* {======================================================================
** Logging
** ======================================================================= */

static void flog (const char *what, const char *fmt, va_list ap) {
  fprintf(stderr, "%s (%s): ", progname, what);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  fflush(stderr);
}

/* more ergonomic way to call 'flog' */
#define dolog(what, fmt) \
  { va_list ap; va_start(ap, fmt); flog(what, fmt, ap); va_end(ap); }

static void loginfo (const char *fmt, ...) {
  dolog("info", fmt);
}

static void logwarn (const char *fmt, ...) {
  dolog("warning", fmt);
}

static void logerror (const char *fmt, ...) {
  dolog("error", fmt);
  fail = 1;
}

#define logGamma(sg, brightness) \
        loginfo("gamma: [R:%g, G:%g, B:%g], brightness: %g", \
                (sg).red, (sg).green, (sg).blue, brightness)

/* }{=====================================================================
** CLI arguments
** ======================================================================= */

/* strcmp for 'argv[i]' */
#define IS(opt)     (strcmp(argv[i], opt) == 0)

/* get the crtc or screen index argument */
static int collectindex (const char *const *argv, int argc, int i) {
  int iscrtc = (IS("-c") || IS("--crtc")) ? 1 : 0;
  i++; /* check next argument for the index */
  if (i < argc) { /* have next argument? */
    if (iscrtc) /* crtc index? */
      crtc_arg = atoi(argv[i]);
    else /* screen index */
      screen_arg = atoi(argv[i]);
    return 1; /* ok */
  } else { /* missing index argument */
    const char *arg = argv[--i];
    const char *what = iscrtc ? "crtc" : "screen";
    logerror("'%s' is missing %s index argument", arg, what);
    return 0; /* fail */
  }
}

/* bits in 'flags' */
#define has_h     1 /* -h or --help */
#define has_d     2 /* -d or --delta */
#define has_t     4 /* -t or --toggle */

/* collect the CLI arguments into 'flags' */
static unsigned collectargs (int argc, const char *const *argv, tempstate *ts) {
  unsigned flags = 0;
  progname = argv[0];
  for (int i = 1; i < argc; i++) {
    if (IS("-h") || IS("--help")) {
      flags |= has_h; /* show usage */
      break; /* done */
    } else if (IS("-v") || IS("--verbose"))
      verbose = 1;
    else if (IS("-d") || IS("--delta"))
      flags |= has_d;
    else if (IS("-t") || IS("--toggle"))
      flags |= has_t;
    else if (IS("-s") || IS("--screen") || IS("-c") || IS("--crtc")) {
      if (!collectindex(argv, argc, i)) { /* missing index argument? */
        flags |= has_h; /* show usage before exit */
        break; /* done */
      }
      i++; /* skip index argument */
    } else if (ts->temp == MIN_DELTA) /* have default temperature? */
      ts->temp = atoi(argv[i]); /* assume argument is temperature */
    else if (ts->brightness == MIN_DELTA) /* have default brightness? */
      ts->brightness = atof(argv[i]); /* assume argument is brightness */
    else { /* unknown argument */
      logerror("unrecognized argument '%s'", argv[i]);
      flags |= has_h;
    }
  }
  return flags;
}

/* }===================================================================== */

static void usage (void) {
  printf("Xsct (%s)\n"
         "Usage: %s [options] [temperature] [brightness]\n"
         "\tIf the argument is 0, xsct resets the display to the default "
         "temperature (6500K)\n"
         "\tIf no arguments are passed, xsct estimates the current display "
         "temperature and brightness\n"
         "Options:\n"
         "\t-h, --help \t xsct will display this usage information\n"
         "\t-v, --verbose \t xsct will display debugging information\n"
         "\t-d, --delta\t xsct will consider temperature and brightness "
         "parameters as relative shifts\n"
         "\t-s, --screen N\t xsct will only select screen specified by given "
         "zero-based index\n"
         "\t-t, --toggle \t xsct will toggle between 'day' and 'night' mode\n"
         "\t-c, --crtc N\t xsct will only select CRTC specified by given "
         "zero-based index\n",
         XSCT_VERSION, progname);
}


static double trimdouble (double x, double a, double b) {
  const double buff[3] = { a, x, b };
  return buff[(int)(x > a) + (int)(x > b)];
}


static int getscreengamma (Display *dpy, int iscreen, int icrtc, sgamma *sg) {
  double gammar = 0.0, gammag = 0.0, gammab = 0.0;
  Window root = RootWindow(dpy, iscreen);
  XRRScreenResources *xrr_res = XRRGetScreenResourcesCurrent(dpy, root);
  int ncrtc = xrr_res->ncrtc;
  if ((unsigned)icrtc < (unsigned)ncrtc) /* crtc index in bounds? */
    ncrtc = 1; /* only get temperature of icrtc */
  else /* otherwise get all temperatures */
    icrtc = 0; /* (start from first) */
  for (int c = icrtc; c < (icrtc + ncrtc); c++) {
    RRCrtc crtcxid = xrr_res->crtcs[c];
    XRRCrtcGamma *xrr_gamma = XRRGetCrtcGamma(dpy, crtcxid);
    int gi = xrr_gamma->size - 1;
    gammar += xrr_gamma->red[gi];
    gammag += xrr_gamma->green[gi];
    gammab += xrr_gamma->blue[gi];
    XRRFreeGamma(xrr_gamma);
  }
  XFree(xrr_res);
  sg->red = gammar;
  sg->green = gammag;
  sg->blue = gammab;
  return ncrtc;
}


/* get screen temp */
static tempstate getst (Display *dpy, int iscreen, int icrtc) {
  sgamma sg = { 0 };
  double t;
  tempstate ts;
  int ncrtc = getscreengamma(dpy, iscreen, icrtc, &sg);
  ts.brightness = MAX(sg.red, sg.green);
  ts.brightness = MAX(sg.blue, ts.brightness);
  if (ts.brightness > 0.0 && ncrtc > 0) { /* need median? */
    double gdelta;
    sg.red /= ts.brightness;
    sg.green /= ts.brightness;
    sg.blue /= ts.brightness;
    ts.brightness /= ncrtc;
    ts.brightness /= BRIGHTNESS_DIV;
    ts.brightness = trimdouble(ts.brightness, 0.0, 1.0);
    if (verbose)
      logGamma(sg, ts.brightness);
    gdelta = sg.blue - sg.red;
    if (gdelta < 0.0) {
      if (sg.blue > 0.0) {
        t = exp((sg.green + 1.0 + gdelta - (GAMMA_K0GR + GAMMA_K0BR)) /
                (GAMMA_K1GR + GAMMA_K1BR)) +
            MINTEMP;
      } else {
        t = (sg.green > 0.0) ?
              (exp((sg.green - GAMMA_K0GR) / GAMMA_K1GR) + MINTEMP) :
              MINTEMP;
      }
    } else {
      t = exp((sg.green + 1.0 - gdelta - (GAMMA_K0GB + GAMMA_K0RB)) /
              (GAMMA_K1GB + GAMMA_K1RB)) +
          (TEMP_NORM - MINTEMP);
    }
  } else {
    t = 0.0;
    ts.brightness = trimdouble(ts.brightness, 0.0, 1.0);
  }
  ts.temp = (long)(t + 0.5);
  return ts;
}


/* set screen temp */
static void setst (Display *dpy, int iscreen, int icrtc, tempstate ts) {
  Window root = RootWindow(dpy, iscreen);
  XRRScreenResources *xrr_res = XRRGetScreenResourcesCurrent(dpy, root);
  double b = trimdouble(ts.brightness, 0.0, 1.0);
  double t = (double)ts.temp;
  int ncrtc = xrr_res->ncrtc;
  sgamma sg = { 0 };
  if (ts.temp < TEMP_NORM) {
    sg.red = 1.0;
    if (ts.temp > MINTEMP) {
      const double g = log(t - MINTEMP);
      sg.green = trimdouble(GAMMA_K0GR + GAMMA_K1GR * g, 0.0, 1.0);
      sg.blue = trimdouble(GAMMA_K0BR + GAMMA_K1BR * g, 0.0, 1.0);
    } else
      sg.green = sg.blue = 0.0;
  } else {
    const double g = log(t - (TEMP_NORM - MINTEMP));
    sg.red = trimdouble(GAMMA_K0RB + GAMMA_K1RB * g, 0.0, 1.0);
    sg.green = trimdouble(GAMMA_K0GB + GAMMA_K1GB * g, 0.0, 1.0);
    sg.blue = 1.0;
  }
  if (verbose)
    logGamma(sg, b);
  if ((unsigned)icrtc < (unsigned)ncrtc)
    ncrtc = 1;
  else
    icrtc = 0;
  for (int c = icrtc; c < (icrtc + ncrtc); c++) {
    RRCrtc crtcxid = xrr_res->crtcs[c];
    int size = XRRGetCrtcGammaSize(dpy, crtcxid);
    XRRCrtcGamma *xrr_crtc_gamma = XRRAllocGamma(size);
    for (int i = 0; i < size; i++) {
      const double g = GAMMA_MULT * b * (double)i / (double)size;
      xrr_crtc_gamma->red[i] = (unsigned short int)(g * sg.red + 0.5);
      xrr_crtc_gamma->green[i] = (unsigned short int)(g * sg.green + 0.5);
      xrr_crtc_gamma->blue[i] = (unsigned short int)(g * sg.blue + 0.5);
    }
    XRRSetCrtcGamma(dpy, crtcxid, xrr_crtc_gamma);
    XRRFreeGamma(xrr_crtc_gamma);
  }
  XFree(xrr_res);
}


/* checks if temperature is in bounds and corrects it if needed */
static long boundtemp (long temp, long dfl, const char *what) {
  if (temp <= 0) {
    logwarn("temperatures of 0 and below cannot be displayed (%s)", what);
    return (dfl < 0) ? TEMP_NORM : dfl;
  } else if (temp < MINTEMP) {
    logwarn("temperatures below %d cannot be displayed (%s)", MINTEMP, what);
    return (dfl < 0) ? MINTEMP : dfl;
  }
  return temp;
}


/* checks if the brightness is in bounds and corrects it if needed */
static double boundbrightness (double brightness) {
  if (brightness < 0.0) {
    logwarn("brightness values below 0.0 cannot be displayed");
    brightness = 0.0;
  } else if (brightness > 1.0) {
    logwarn("brightness values above 1.0 cannot be displayed");
    brightness = 1.0;
  }
  return brightness;
}


/* checks the bounds of tempstate members and corrects them if needed */
static void boundts (tempstate *const ts, const char *twhat) {
  boundtemp(ts->temp, -1, twhat);
  boundbrightness(ts->brightness);
}


/* open connection to the X server */
static Display *opendisplay (void) {
  Display *dpy;
  errno = 0;
  if (!(dpy = XOpenDisplay(NULL))) { /* connection failed? */
    const char *msg = "could not open a connection to the X server";
    if (errno != 0)
      logerror("%s: %s", msg, strerror(errno));
    else
      logerror("%s", msg);
    loginfo("ensure DISPLAY environment variable is set correctly");
    exit(EXIT_FAILURE);
  }
  return dpy;
}


static void errorargscreen (int nscreen) {
  if (nscreen > 1) /* multiple screens? */
    logerror("invalid screen index '%d', expected 0..%d", screen_arg, nscreen);
  else /* only one screen */
    logerror("invalid screen index '%d', expected 0");
}


static long envtotemp (const char *p, long dfl, const char *envn) {
  char *endptr = NULL;
  long x = strtol(p, &endptr, 10);
  if (endptr == p) { /* nothing was converted? */
    logwarn("invalid value for %s environment variable (expect integer)", envn);
    x = dfl; /* set default value */
  } /* else something was converted */
  return boundtemp(x, dfl, envn);
}


/*
** Check the environment for the variables XSCT_TEMPERATURE_DAY and
** XSCT_TEMPERATURE_NIGHT and use them as defaults (if any).
** This also corrects then if they are out of some hard bounds.
*/
static void checkenvironment (void) {
  char *p;
  if ((p = getenv(XSCT_TEMPERATURE_DAY)))
    temp_day = envtotemp(p, temp_day, XSCT_TEMPERATURE_DAY);
  if ((p = getenv(XSCT_TEMPERATURE_NIGHT)))
    temp_night = envtotemp(p, temp_night, XSCT_TEMPERATURE_NIGHT);
}


/* variation in screen temperature for 'toggledaynight' */
#if !defined(TOGGLE_DELTA)
#define TOGGLE_DELTA        200
#endif

/*
** Toggles the temperature between temp_night/temp_day for screens
** in the interval [0, nscren).
*/
static void toggledaynight (Display *dpy, int nscreen) {
  for (int i = 0; i < nscreen; i++) {
    tempstate ts = getst(dpy, i, crtc_arg);
    if (ts.temp > (temp_day - TOGGLE_DELTA))
      ts.temp = temp_night;
    else
      ts.temp = temp_day;
    setst(dpy, i, crtc_arg, ts);
  }
}


static void printestimate (Display *dpy, int first, int last) {
  while (first <= last) {
    tempstate ts = getst(dpy, first, crtc_arg);
    printf("Screen[%d]: temperature ~ %ld %g\n", first, ts.temp, ts.brightness);
    first++;
  }
}


static void regularsct (Display *dpy, tempstate ts, int first, int last) {
  if (ts.temp == 0) /* set default value? */
    ts.temp = temp_day;
  else
    boundts(&ts, "specified by user");
  for (int i = first; i <= last; i++) /* for each screen... */
    setst(dpy, i, crtc_arg, ts); /* set temp */
}


static void deltasct (Display *dpy, tempstate ts, int first, int last) {
  if (ts.temp == MIN_DELTA || ts.brightness == MIN_DELTA)
    logerror("temperature and brightness delta must both be specified");
  else { /* shift temperature and optionally brightness */
    for (int i = first; i <= last; i++) { /* for each screen... */
      tempstate dts = getst(dpy, i, crtc_arg);
      dts.temp += ts.temp;
      dts.brightness += ts.brightness;
      boundts(&dts, "specified by user");
      setst(dpy, i, crtc_arg, dts);
    }
  }
}


int main (int argc, const char *const *argv) {
  tempstate ts = { .temp = MIN_DELTA, .brightness = MIN_DELTA };
  Display *dpy = opendisplay();
  int nscreen = XScreenCount(dpy);
  unsigned flags = collectargs(argc, argv, &ts);
  checkenvironment(); /* (this might change default values) */
  if (flags & has_h) /* have -h or --help ? */
    usage();
  else if (!fail && screen_arg >= nscreen) /* invalid screen specified? */
    errorargscreen(nscreen);
  else {
    int firstscreen = 0;
    int lastscreen = nscreen - 1;
    if (flags & has_t) /* -t or --toggle? */
      toggledaynight(dpy, nscreen);
    if ((ts.brightness == MIN_DELTA) && !(flags & has_d))
      ts.brightness = 1.0; /* set default brightness */
    if (screen_arg >= 0) { /* screen index was specified? */
      firstscreen = screen_arg;
      lastscreen = screen_arg;
    }
    if ((ts.temp == MIN_DELTA) && !(flags & has_d)) /* no arguments? */
      printestimate(dpy, firstscreen, lastscreen);
    else { /* unspecified temperature or delta mode */
      if (!(flags & has_d)) /* absolute mode? */
        regularsct(dpy, ts, firstscreen, lastscreen);
      else /* otherwise delta mode */
        deltasct(dpy, ts, firstscreen, lastscreen);
    }
  }
  XCloseDisplay(dpy);
  return (fail) ? EXIT_FAILURE : EXIT_SUCCESS;
}
