.PHONY: test dist

all:
	$(MAKE) dist
	$(MAKE) test
	$(MAKE) -C examples clean
	$(MAKE) -C examples
	rm -rf my-suite
	PYTHONPATH=. python3 -m nala init my-suite
	$(MAKE) -C my-suite NALA="PYTHONPATH=.. python3 -m nala"
	test -f my-suite/report.json
	! test -f my-suite/build/report.json
	rm my-suite/report.json
	$(MAKE) -C my-suite \
	    NALA="PYTHONPATH=.. python3 -m nala" \
	    ARGS="--report-json-file build/report.json"
	! test -f my-suite/report.json
	test -f my-suite/build/report.json
	rm my-suite/build/report.json
	! $(MAKE) -C my-suite \
	    NALA="PYTHONPATH=.. python3 -m nala" \
	    ARGS="--report-json-file foo/report.json"
	$(MAKE) -C my-suite NALA="PYTHONPATH=.. python3 -m nala" clean
	$(MAKE) -C my-suite NALA="PYTHONPATH=.. python3 -m nala" SANITIZE=yes

test: test-python test-c

test-python:
	python3 setup.py test

test-c:
	$(MAKE) -C tst

dist:
	amalgamate/amalgamate.py -c amalgamate/nala.h.json -s .
	amalgamate/amalgamate.py -c amalgamate/nala.c.json -s .

release-to-pypi:
	python setup.py sdist
	python setup.py bdist_wheel --universal
	twine upload dist/*.gz dist/*.whl
