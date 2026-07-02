/* hello.rexx - HTTPREXX direct-execution sample.
   The SAY output is the HTTP response body. A query string reaches the
   program via PARSE ARG (e.g. ?name=Mike). */
parse arg name
if name = '' then name = 'World'
say '<html><body>'
say '<h1>Hello, ' || name || '!</h1>'
say '<p>Rendered by a .rexx script under HTTPREXX.</p>'
say '</body></html>'
