# Setup.
AC_INIT([mod_websocket], [0.1.0])
AC_CONFIG_SRCDIR([mod_websocket.c])

# Declare optional user-defined variables.
AC_ARG_VAR([APACHE_BINDIR],
           [the directory containing the binaries for your httpd installation
            (if you have a non-standard installation path, setting this value is
            easier than setting APXS and APACHECTL separately)])

AC_ARG_VAR([APXS], [the path to the Apache apxs@<:@2@:>@ tool])

AC_ARG_VAR([APACHECTL], [the path to the apachectl tool])

# Sanity-check user-defined variables.
AS_IF([test \( "x$APACHE_BINDIR" != "x" \) -a \( ! -d "$APACHE_BINDIR" \)],
      [AC_MSG_WARN([APACHE_BINDIR value $APACHE_BINDIR does not appear to be a valid directory])])

AS_IF([test \( "x$APXS" != "x" \) -a \( ! -x "$APXS" \)],
      [AC_MSG_WARN([APXS value $APXS does not appear to be an executable file])])

AS_IF([test \( "x$APACHECTL" != "x" \) -a \( ! -x "$APACHECTL" \)],
      [AC_MSG_WARN([APACHECTL value $APACHECTL does not appear to be an executable file])])

# Find the programs.
AS_IF([test "x$APACHE_BINDIR" != "x"], [SEARCHPATH=$APACHE_BINDIR:$PATH],
      [SEARCHPATH=$PATH])

AC_PATH_PROGS([APXS], [apxs apxs2], [no], [$SEARCHPATH])
AS_IF([test "x$APXS" = "xno"],
      [AC_MSG_ERROR([could not find the apxs tool])])

AC_PATH_PROG([APACHECTL], [apachectl], [no], [$SEARCHPATH])
AS_IF([test "x$APACHECTL" = "xno"],
      [AC_MSG_ERROR([could not find the apachectl tool])])

# Define the template-generated files.
AC_CONFIG_FILES([
  Makefile
])

# Output everything.
AC_OUTPUT