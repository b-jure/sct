# About

Fork of the original project:
https://github.com/faf0/sct

Xsct (X11 set color temperature) is a UNIX tool which allows you to set the color
temperature of your screen. It is simpler than [Redshift](https://github.com/jonls/redshift) and [f.lux](https://justgetflux.com/).

Original code was published by Ted Unangst in the public domain:
https://www.tedunangst.com/flak/post/sct-set-color-temperature

# Installation

## Make-based

On UNIX-based systems, a convenient method of building this software is using Make.
Since the `Makefile` is simple and portable, both the BSD and [GNU make](https://www.gnu.org/software/make/) variants will
have no problems parsing and executing it correctly.

The simplest invocation is
~~~sh
make
~~~
that will use the default values for all flags as provided in the `Makefile`.

Overriding any of the following variables is supported by exporting a variable
with the same name and your desired content to the environment:
* `CC`
* `CFLAGS`
* `LDFLAGS`
* `PREFIX`
* `BIN` (the directory into which the resulting binary will be installed)
* `MAN` (the directory into which the man page will be installed)
* `INSTALL` (`install(1)` program used to create directories and copy files)

Both example calls are equivalent and will build the software with the specified compiler and custom compiler flags:
~~~sh
make CC='clang' CFLAGS='-O2 -pipe -g3 -ggdb3' LDFLAGS='-L/very/special/directory -flto'
~~~

~~~sh
export CC='clang'
export CFLAGS='-O2 -pipe -g3 -ggdb3'
export LDFLAGS='-L/very/special/directory -flto'
make
~~~

The software can be installed by running the following command:
~~~sh
make install
~~~

If you prefer a different installation location, override the `PREFIX` variable:
~~~sh
make install PREFIX="${HOME}/xsct-prefix"
~~~

~~~sh
export PREFIX="${HOME}/xsct-prefix"
make install
~~~

## Manual compilation

Compile the code using the following command:
~~~sh
gcc -Wall -Wextra -Werror -pedantic -std=c99 -O2 -I /usr/X11R6/include xsct.c -o xsct -L /usr/X11R6/lib -lX11 -lXrandr -lm -s
~~~

# Quirks

If the delta mode is used to decrease the brightness to below 0.0 and then
increased above 0.0, the temperature will reset to 6500 K, regardless of
whatever it was before the brightness reached 0.
This is because the temperature is reset to 0 K when the brightness is set equal
to or below 0.0 (to verify this, you can run `xsct 0 0.0; xsct`).

# Resources

The following website by Mitchell Charity provides a table for the conversion
between black-body temperatures and color pixel values:
http://www.vendian.org/mncharity/dir3/blackbody/
