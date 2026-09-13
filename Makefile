.PHONY: help install install-dev test test-all test-cov lint format type-check quality clean build-rx rx-test rx-test-all rx-selftest rx-stereo-selftest rx-sweep rx-snr-cal rx-demod-cal rx-scan pre-commit ci

help: ## Show this help message
	@echo 'Usage: make [target]'
	@echo ''
	@echo 'Targets:'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z_-]+:.*?## / {printf "  %-15s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

# webui/ targets (Python, FastAPI listener interface)

install: ## Install webui package
	pip install -e webui/

install-dev: ## Install webui with dev dependencies
	pip install -e "webui/[dev]"

test: ## Run webui test suite (~2 s)
	pytest -c webui/pyproject.toml

test-all: ## Run all webui tests (~2 s)
	pytest -c webui/pyproject.toml -m ""

test-cov: ## Run tests with coverage report
	pytest -c webui/pyproject.toml --cov=webui --cov-report=html --cov-report=term-missing

lint: ## Lint webui and tests
	flake8 webui/ webui/tests/

format: ## Format webui and tests with black/isort
	black webui/ webui/tests/
	isort webui/ webui/tests/

type-check: ## Type-check webui with mypy
	mypy webui/

quality: lint type-check ## Run all code quality checks

# rx/ targets (C++, FM receiver)

build-rx: ## Build feedmyfm-rx binary (release mode)
	cmake -S rx -B rx/build -DCMAKE_BUILD_TYPE=Release
	cmake --build rx/build -j

rx-test: build-rx ## Run rx CTest suite (fast tests only)
	cd rx/build && ctest --output-on-failure -LE slow

rx-test-all: build-rx ## Run rx CTest suite (all tests, including slow ~100s)
	cd rx/build && ctest --output-on-failure

# The synthetic (no-SDR) modes below only need a *valid* config + station
# list to build a Plan -- they don't touch a real SDR. Default to the same
# rx/tests/ mock fixtures CTest uses (never a real deployment's config.yml /
# stations.yml); override the *_CFG / *_STATIONS vars to aim a target at a
# live deployment's files instead, e.g. `make rx-selftest RX_CFG=deploy/config.yml
# RX_STATIONS=deploy/stations.yml`.
RX_CFG      ?= rx/tests/config.yml
RX_STATIONS ?= rx/tests/stations.yml
RX_STEREO_STATIONS ?= rx/tests/stations-stereo.yml
RX_SCAN_CFG      ?= rx/tests/config-scan.yml
RX_SCAN_STATIONS ?= rx/tests/stations-scan.yml

rx-selftest: build-rx ## Run rx no-SDR mono DSP self-test
	./rx/build/feedmyfm-rx -c $(RX_CFG) -s $(RX_STATIONS) --selftest

rx-stereo-selftest: build-rx ## Run rx stereo multiplex decode / separation self-test
	./rx/build/feedmyfm-rx -c $(RX_CFG) -s $(RX_STEREO_STATIONS) --stereo-selftest

rx-sweep: build-rx ## Print rx audio frequency response vs ideal de-emphasis
	./rx/build/feedmyfm-rx -c $(RX_CFG) -s $(RX_STATIONS) --sweep

rx-snr-cal: build-rx ## Sweep rx SNR calibration (C/N vs recovered audio SNR)
	./rx/build/feedmyfm-rx -c $(RX_CFG) -s $(RX_STATIONS) --snr-cal

rx-demod-cal: build-rx ## Compare FM demod options across the threshold knee
	./rx/build/feedmyfm-rx -c $(RX_CFG) -s $(RX_STATIONS) --demod-cal

rx-scan: build-rx ## Run rx passive band-scan periodogram on synthetic multiplex
	./rx/build/feedmyfm-rx -c $(RX_SCAN_CFG) -s $(RX_SCAN_STATIONS) --scan

# Maintenance targets

clean: ## Clean build artifacts
	rm -rf rx/build/
	rm -rf .coverage
	rm -rf htmlcov/
	rm -rf .pytest_cache/
	rm -rf .mypy_cache/
	find . -type d -name __pycache__ -exec rm -rf {} +
	find . -type f -name "*.pyc" -delete

pre-commit: ## Run pre-commit on all files
	pre-commit run --all-files

ci: quality test-cov rx-test-all ## Run all CI checks (webui + rx tests)
