# Clickable links (#39)

Generate the test PDF with `python tests/create_link_fixture.py` and open
`out/link-tests/clickable-links.pdf` in the application. It has three pages,
four supported links, blocked file and Windows-settings links, and a broken
page destination. Use a larger real PDF with a linked table of contents as well.

- Hover over each supported link: the cursor becomes a hand and returns to the
  normal cursor outside its rectangle. File, settings and broken links have no
  hand cursor or active area. Clicking them does nothing.
- Click the internal link: the view scrolls to the top of page 3. Repeat in
  single-column and book view. Named destinations in a real table of contents
  also navigate to their target pages.
- Click HTTP and HTTPS: they open in the default browser. Click mailto: it
  opens the default mail app. Right-clicking does not activate a link.
- Check the hit areas after zooming, all quarter-turn rotations, resizing,
  switching tabs, and moving the window to a monitor with different DPI.
- Scroll the wheel over a link and over empty page content; both scroll the
  document. Drag the scrollbar and pan with touch over links without opening
  them accidentally. An actual tap activates the link.
- Rapidly zoom or rotate while pages first appear, then stop. Links must appear
  without another scroll, resize or view change to trigger a retry.
- Move a tab to another window while its pages are first rendering, then close
  the source window. Links must appear in the destination window. Move it back
  and repeat; a late result from an earlier owner must not overwrite the cache.
- On an already loaded page, revisit it after zoom, rotation or transfer: links
  stay active and do not depend on rerunning MuPDF link extraction.
- Scroll quickly through a large PDF: rendering stays responsive. Close a tab
  and close the app while link loading is active: no crash or stale areas on
  a newly opened document.

Keyboard activation, Ctrl+click, tooltips, and scrolling to a destination's
exact x/y position are outside this issue's scope.
