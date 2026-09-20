# ICDE paper draft

This directory contains the maintainable TeX and BibTeX sources of the early
ICDE-style paper draft recovered from the documentation archive. Generated
PDF/auxiliary files, the Overleaf bundle, and the original template ZIP are
not tracked; install the standard IEEEtran package from your TeX distribution.

## Build

```bash
cd docs/icde-paper
pdflatex main.tex
bibtex main
pdflatex main.tex
pdflatex main.tex
```

On Debian/Ubuntu, `IEEEtran.cls` is provided by `texlive-publishers`.

References are maintained in `references.bib` and rendered with the
`IEEEtran` BibTeX style. Overleaf runs the required BibTeX passes
automatically when the project is recompiled.

The current draft is intentionally conservative:

- numbers in the evaluation section are marked preliminary because the
  io_uring and SPDK runs were collected on different dates;
- planned metadata and shared-poller work is presented as design/future work;
- author, affiliation, funding, final references, figures, and a controlled
  evaluation matrix remain TODOs.

## Suggested path to submission

1. Freeze the system contribution and terminology around the common I/O
   session, buffer ownership, and bounded prefetch pipeline.
2. Finish the common API and cross-file asynchronous metadata implementation.
3. Rerun all baselines and ablations from one commit and one machine state.
4. Add architecture, timeline, and evaluation figures generated from the
   reproducible result set.
5. Expand related work, add the Pixels publication, and verify every citation.
