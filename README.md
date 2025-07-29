# Switch-NRO-Launcher

This project builds a simple Nintendo Switch homebrew that lists GitLab releases
and allows downloading assets directly on the console.

## Token configuration

The application requires a GitLab personal access token to access the release
API. Edit `source/token.h` and replace the placeholder string with your token
before building. The token is compiled into the application.

## Building

The project is built using `devkitARM` from the [devkitPro](https://devkitpro.org)
set of tools. Ensure the environment variable `DEVKITPRO` points to your
installation before running `make`:

```bash
export DEVKITPRO=/opt/devkitpro
make
```

## Running

Copy the resulting `.nro` to your Switch SD card and launch it via the Homebrew
Menu. The application will attempt to fetch release information using the token
configured as described above.
