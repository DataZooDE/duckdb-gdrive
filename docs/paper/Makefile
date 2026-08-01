# Build the paper. `make` here is independent of the extension build.
#
# The acmart class is vendored under cls/ rather than assumed present:
# texlive-publishers is not in every distribution's base install, and a paper
# that only builds on the author's machine is a paper nobody else can check.

TEXINPUTS := ./cls//:
BSTINPUTS := ./cls//:
export TEXINPUTS BSTINPUTS

.PHONY: all clean pages watch figures

all: paper.pdf

# Regenerate both figures from the committed measurement data. This does NOT
# re-measure -- see `make latency_breakdown` and `make trace_figure` in the
# repo root for that, both of which need live Drive credentials. Splitting the
# two means a paper rebuild is offline and deterministic.
figures:
	cd ../../e2e && uv run --frozen python -m helpers.plot_breakdown \
	    ../docs/paper/data_breakdown.json > ../docs/paper/fig_breakdown.tex
	cd ../../e2e && uv run --frozen python -m helpers.plot_timeline \
	    ../docs/paper/fig_timeline.tex \
	    ../docs/paper/data/trace_scan.jsonl "(a) cold Parquet scan, 87\\,MB, 16\\,MiB blocks" \
	    ../docs/paper/data/trace_ducklake_read.jsonl "(b) DuckLake read (cold): count/sum/count-distinct over 200k rows" \
	    ../docs/paper/data/trace_ducklake_write.jsonl "(c) DuckLake write: CREATE TABLE AS, 200k rows"

paper.pdf: paper.tex refs.bib fig_breakdown.tex fig_timeline.tex
	pdflatex -interaction=nonstopmode -halt-on-error paper.tex
	-bibtex paper
	pdflatex -interaction=nonstopmode -halt-on-error paper.tex
	pdflatex -interaction=nonstopmode -halt-on-error paper.tex
	@$(MAKE) --no-print-directory pages

# The page budget is 8-10. Print it after every build so it cannot drift
# unnoticed while sections are being written.
pages: paper.pdf
	@n=$$(pdfinfo paper.pdf 2>/dev/null | awk '/^Pages:/ {print $$2}'); \
	 echo "==> $$n pages (budget 8-10)"

clean:
	rm -f paper.pdf paper.aux paper.log paper.out paper.bbl paper.blg
