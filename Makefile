PYTHON ?= python3
.PHONY: verify summary figures tvla plan native paper
verify:
	$(PYTHON) scripts/reproduce.py verify
summary:
	$(PYTHON) scripts/reproduce.py summary
figures:
	$(PYTHON) scripts/reproduce.py figures
tvla:
	$(PYTHON) scripts/reproduce.py tvla
plan:
	$(PYTHON) scripts/reproduce.py plan
native:
	$(PYTHON) scripts/reproduce.py native --openroad "$(OPENROAD)"
paper:
	cd paper && pdflatex -interaction=nonstopmode -halt-on-error main.tex
	cd paper && pdflatex -interaction=nonstopmode -halt-on-error main.tex
