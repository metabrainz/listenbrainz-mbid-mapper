"""
Locust load testing module for the fuzzy mapper service.

This module provides load testing capabilities for the mapping lookup API
using the Locust framework.

Usage:
    locust -f locustfile.py --host http://localhost:5000
    
    Or with web UI:
    locust -f locustfile.py --host http://localhost:5000 --web-host 0.0.0.0

Environment variables:
    MAPPER_HOST: Base URL of the mapper service (default: http://localhost:5000)
    TEST_DATA_FILE: Path to JSONL file with listens (default: ../listen_test_data/listens.jsonl)
"""

import json
import logging
import os
import random
from locust import HttpUser, task, between, events
from locust.runners import MasterRunner

logger = logging.getLogger(__name__)

# Configuration
DEFAULT_HOST = os.environ.get("MAPPER_HOST", "http://localhost:5000")
TEST_DATA_FILE = os.environ.get("TEST_DATA_FILE", "../listen_test_data/listens.jsonl")


def load_all_test_cases(filepath: str) -> list:
    """
    Load all test cases from a JSONL file into memory.
    
    JSONL format: {"track_metadata": {"artist_name": "...", "release_name": "...", "track_name": "..."}, ...}
    
    Returns a list of all test cases.
    """
    test_cases = []
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                track_metadata = data.get("track_metadata", {})
                test_cases.append({
                    "artist_credit_name": track_metadata.get("artist_name", ""),
                    "release_name": track_metadata.get("release_name", ""),
                    "recording_name": track_metadata.get("track_name", "")
                })
            except json.JSONDecodeError:
                continue  # Skip malformed lines
    return test_cases


class TestCaseIterator:
    """
    Iterates through ALL test cases before repeating.
    
    Loads all test cases into memory once, shuffles them, and iterates
    through the entire dataset before reshuffling and repeating.
    Thread-safe for use by multiple Locust users.
    """
    
    def __init__(self, filepath: str):
        self.filepath = filepath
        self.test_cases = []
        self._index = 0
        self._load_and_shuffle()
    
    def _load_and_shuffle(self):
        """Load all test cases and shuffle them."""
        try:
            self.test_cases = load_all_test_cases(self.filepath)
            random.shuffle(self.test_cases)
            self._index = 0
            logger.info(f"Loaded {len(self.test_cases):,} test cases from {self.filepath}")
        except FileNotFoundError:
            logger.warning(f"Test data file not found: {self.filepath}")
            self.test_cases = []
    
    def get_next(self) -> dict:
        """Get the next test case, reshuffling when all have been used."""
        if not self.test_cases:
            return None
        
        # Get current item
        item = self.test_cases[self._index]
        self._index += 1
        
        # Reshuffle when we've gone through all cases
        if self._index >= len(self.test_cases):
            random.shuffle(self.test_cases)
            self._index = 0
            logger.info(f"Completed full iteration of {len(self.test_cases):,} test cases, reshuffling...")
        
        return item


# Shared iterator across all users (loaded once)
_shared_iterator = None


def get_shared_iterator() -> TestCaseIterator:
    """Get or create the shared test case iterator."""
    global _shared_iterator
    if _shared_iterator is None:
        test_file = os.path.join(os.path.dirname(__file__), TEST_DATA_FILE)
        if os.path.exists(test_file):
            _shared_iterator = TestCaseIterator(test_file)
    return _shared_iterator


class MappingUser(HttpUser):
    """
    Simulates a user making mapping lookup requests.
    
    This user class performs fuzzy mapping lookups against the server's
    /mapping/lookup endpoint with various test cases.
    """
    
    # Wait between 0.5 and 2 seconds between requests
    wait_time = between(0.5, 2.0)
    
    # Default host (can be overridden via command line)
    host = DEFAULT_HOST
    
    def on_start(self):
        """Called when a simulated user starts."""
        # Use shared iterator if available, otherwise fall back to defaults
        self._iterator = get_shared_iterator()
        self._fallback_cases = self._get_fallback_cases()
    
    def _get_fallback_cases(self) -> list:
        """Fallback test cases when no CSV file is available."""
        return [
            {
                "artist_credit_name": "The Beatles",
                "release_name": "Abbey Road",
                "recording_name": "Come Together"
            },
            {
                "artist_credit_name": "Pink Floyd",
                "release_name": "The Dark Side of the Moon",
                "recording_name": "Time"
            },
            {
                "artist_credit_name": "Led Zeppelin",
                "release_name": "Led Zeppelin IV",
                "recording_name": "Stairway to Heaven"
            },
            {
                "artist_credit_name": "Queen",
                "release_name": "A Night at the Opera",
                "recording_name": "Bohemian Rhapsody"
            },
            {
                "artist_credit_name": "Radiohead",
                "release_name": "OK Computer",
                "recording_name": "Paranoid Android"
            },
        ]
    
    def _get_next_test_case(self) -> dict:
        """Get the next test case from the iterator or fallback."""
        if self._iterator:
            case = self._iterator.get_next()
            if case:
                return case
        
        # Fallback to default cases
        return random.choice(self._fallback_cases)
    
    @task(10)
    def lookup_with_release(self):
        """
        Perform a mapping lookup with artist, release, and recording.
        
        This is the most common type of lookup with all fields populated.
        """
        test_case = self._get_next_test_case()
        
        params = {
            "artist_credit_name": test_case["artist_credit_name"],
            "recording_name": test_case["recording_name"]
        }
        
        if "release_name" in test_case and test_case["release_name"]:
            params["release_name"] = test_case["release_name"]
        
        with self.client.get(
            "/mapping/lookup",
            params=params,
            name="/mapping/lookup (with release)",
            catch_response=True
        ) as response:
            if response.status_code == 200:
                response.success()
            elif response.status_code == 404:
                # No match found is a valid response
                response.success()
            elif response.status_code == 503:
                # Server still starting up
                response.failure("Server not ready")
            else:
                response.failure(f"Unexpected status: {response.status_code}")
    
    @task(5)
    def lookup_without_release(self):
        """
        Perform a mapping lookup with only artist and recording (no release).
        
        Tests the lookup behavior when release information is not available.
        """
        test_case = self._get_next_test_case()
        
        params = {
            "artist_credit_name": test_case["artist_credit_name"],
            "recording_name": test_case["recording_name"]
        }
        
        with self.client.get(
            "/mapping/lookup",
            params=params,
            name="/mapping/lookup (no release)",
            catch_response=True
        ) as response:
            if response.status_code == 200:
                response.success()
            elif response.status_code == 404:
                response.success()
            elif response.status_code == 503:
                response.failure("Server not ready")
            else:
                response.failure(f"Unexpected status: {response.status_code}")
    
    @task(1)
    def check_health(self):
        """
        Check if the server is responding (hit the root endpoint).
        
        Lower weight as this is just a health check.
        """
        with self.client.get("/", name="/", catch_response=True) as response:
            if response.status_code == 200:
                response.success()
            elif response.status_code == 503:
                response.failure("Server not ready")
            else:
                response.failure(f"Unexpected status: {response.status_code}")


class HighLoadUser(MappingUser):
    """
    Simulates a high-frequency user for stress testing.
    
    This user makes requests much more frequently than a normal user.
    """
    
    wait_time = between(0.1, 0.5)


# Event handlers for custom reporting
@events.test_start.add_listener
def on_test_start(environment, **kwargs):
    """Called when load test starts."""
    if isinstance(environment.runner, MasterRunner):
        print("Load test starting on master node")
    else:
        print(f"Load test starting, targeting: {environment.host}")


@events.test_stop.add_listener
def on_test_stop(environment, **kwargs):
    """Called when load test stops."""
    print("Load test complete")


@events.request.add_listener
def on_request(request_type, name, response_time, response_length, response, 
               context, exception, start_time, url, **kwargs):
    """
    Called on each request completion.
    
    Can be used for custom logging or metrics collection.
    """
    # Placeholder for custom request handling
    # Example: Log slow requests
    # if response_time > 1000:  # More than 1 second
    #     print(f"Slow request: {name} took {response_time}ms")
    pass


if __name__ == "__main__":
    # This allows running directly with: python locustfile.py
    # But typically you'd use: locust -f locustfile.py
    import sys
    print("Run this file using locust:")
    print("  locust -f locustfile.py --host http://localhost:5000")
    print("")
    print("Or with the web UI:")
    print("  locust -f locustfile.py --host http://localhost:5000 --web-host 0.0.0.0")
    sys.exit(0)
