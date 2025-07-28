# Switch-NRO-Launcher

This project builds a simple Nintendo Switch homebrew that lists GitLab releases
and allows downloading assets directly on the console.

## Token configuration

The application requires a GitLab personal access token to access the release
API. You can provide the token in two ways:

1. **Environment variable**: set `GITLAB_PRIVATE_TOKEN` when launching the
   application or building the project.
2. **Compile-time token**: edit `source/token.h` and replace the placeholder
   string with your token.

Providing the token via environment variable avoids hardcoding secrets in the
source tree.

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
