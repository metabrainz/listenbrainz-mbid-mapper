# Load Testing for Fuzzy Mapper

This directory contains load testing tools for the fuzzy mapper service using [Locust](https://locust.io/).

## Setup

1. Install dependencies:
   ```bash
   pip install -r requirements.txt
   ```

2. Make sure the mapper server is running:
   ```bash
   # From the project root
   ./build/server -i /path/to/index
   ```

## Running Load Tests

### With Web UI (Interactive)

```bash
locust -f locustfile.py --host http://localhost:5000
```

Then open http://localhost:8089 in your browser to configure and start the test.

### Headless Mode (CI/Automation)

```bash
# Run with 100 users, spawn rate of 10 users/second, for 5 minutes
locust -f locustfile.py \
    --host http://localhost:5000 \
    --headless \
    --users 100 \
    --spawn-rate 10 \
    --run-time 5m
```

### Distributed Mode (Multiple Workers)

For higher load, run Locust in distributed mode:

**Master:**
```bash
locust -f locustfile.py --master --host http://localhost:5000
```

**Workers:**
```bash
locust -f locustfile.py --worker --master-host localhost
```

## Configuration

### Environment Variables

- `MAPPER_HOST`: Base URL of the mapper service (default: `http://localhost:5000`)
- `TEST_DATA_FILE`: Path to CSV file with test cases (for future use)

### User Classes

- `MappingUser`: Normal user with 0.5-2 second wait between requests
- `HighLoadUser`: Stress test user with 0.1-0.5 second wait between requests

### Test Weights

The default `MappingUser` has the following task weights:
- `lookup_with_release` (weight: 10): Lookup with artist, release, and recording
- `lookup_without_release` (weight: 5): Lookup with only artist and recording
- `check_health` (weight: 1): Simple health check on root endpoint

## Test Cases

Currently uses placeholder test cases. The next step is to integrate real test data from the mapping database.

## Output

Locust provides:
- Real-time statistics in the web UI
- CSV reports (with `--csv` flag)
- JSON statistics via API (http://localhost:8089/stats/requests)

### Example CSV Export

```bash
locust -f locustfile.py \
    --host http://localhost:5000 \
    --headless \
    --users 50 \
    --spawn-rate 5 \
    --run-time 2m \
    --csv=results/load_test
```

This creates:
- `results/load_test_stats.csv` - Request statistics
- `results/load_test_stats_history.csv` - Stats over time
- `results/load_test_failures.csv` - Failed requests
- `results/load_test_exceptions.csv` - Exceptions
