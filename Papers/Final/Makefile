DOC = final

all:
	pdflatex $(DOC)
	bibtex $(DOC)
	pdflatex $(DOC)	
	pdflatex $(DOC)		

clean:
	rm -f $(DOC).pdf *.aux *.log *.ps *.dvi *.bbl *.blg *~* *#*
