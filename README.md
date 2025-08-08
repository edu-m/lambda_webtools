# lambda_webtools

Tools for my blog.

**mdserve** is a small UNIX-style HTTP server that serves Markdown files as HTML, using an external parser.
It supports clean URLs, automatic index rendering, and related articles navigation.
Its main features are:

-  Clean URLs: /dir -> redirects to /dir/
-  Automatically renders index.md or the first .md in a folder
-  Recursively lists related subfolders on the front page with a hierarchical structure
-  Only immediate subfolders shown in related list for subpages
-  Works with any external Markdown-to-HTML parser, you can literally choose whatever you like

**mdparse** is a minimal Markdown-to-HTML converter designed to work with mdserve.
It reads Markdown from stdin and writes HTML to stdout.
Its main features are:

- Converts headings (#, ##, …) to <h1>...</h1>, <h2>...</h2> etc.
- Supports bold (**text**) and italic (*text*)
- Converts \[link text\](url) to <a href="url">link text</a>
- Allows literal [ ] ( ) * in text by escaping them with a backslash like you'd normally do
- Escapes HTML special characters (&, <, >, ") so it doesn't accidentally break the text
- Leaves unsupported Markdown syntax untouched, wrapped in <p>