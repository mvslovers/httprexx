# CLAUDE.md — httprexx

## Project Overview

HTTPREXX is the REXX CGI handler for the HTTPD web server on MVS 3.8j. It was
extracted from the HTTPD codebase (issue mvslovers/httpd#61) into a standalone
project to decouple the BREXX runtime dependency from the HTTP server core.

HTTPREXX produces two load modules:
- **HTTPSAY** — BREXX say helper (minimal CRT, ADDRESS LINKMVS interface)
- **HTTPREXX** — REXX CGI handler (standard CRT, loads HTTPSAY at runtime)

Both are registered as external CGI modules in HTTPD's Parmlib configuration:
```
CGI HTTPREXX /rexx/*
```

## C Standard

This project uses `-std=gnu99` (same as HTTPD).

## Dependencies

- **crent370** — C runtime
- **httpd** — for httpcgi.h (CGI module interface)
