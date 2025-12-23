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
    TEST_DATA_FILE: Path to JSONL file with listens (default: ../listens_test_data/listens.jsonl)
"""

import json
import os
import random
from itertools import islice
from locust import HttpUser, task, between, events
from locust.runners import MasterRunner


# Configuration
DEFAULT_HOST = os.environ.get("MAPPER_HOST", "http://localhost:5000")
TEST_DATA_FILE = os.environ.get("TEST_DATA_FILE", "../listens/listens.jsonl")
BUFFER_SIZE = int(os.environ.get("TEST_BUFFER_SIZE", "1000"))


def test_case_generator(filepath: str):
    """
    Generator that yields test cases from a JSONL file one line at a time.
    
    JSONL format: {"track_metadata": {"artist_name": "...", "release_name": "...", "track_name": "..."}, ...}
    """
    with open(filepath, 'r', encoding='utf-8') as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                data = json.loads(line)
                track_metadata = data.get("track_metadata", {})
                print("%s, %s, %s" % (track_metadata.get("artist_name", ""), track_metadata.get("release_name", ""), track_metadata.get("track_name", "")))
                yield {
                    "artist_credit_name": track_metadata.get("artist_name", ""),
                    "release_name": track_metadata.get("release_name", ""),
                    "recording_name": track_metadata.get("track_name", "")
                }
            except json.JSONDecodeError:
                continue  # Skip malformed lines


class TestCaseBuffer:
    """
    Maintains a rotating buffer of test cases for random sampling.
    
    Loads test cases incrementally and maintains a fixed-size buffer
    for random selection without loading the entire dataset.
    """
    
    def __init__(self, filepath: str, buffer_size: int = 1000):
        self.filepath = filepath
        self.buffer_size = buffer_size
        self.buffer = []
        self._generator = None
        self._exhausted = False
        self._refill_buffer()
    
    def _refill_buffer(self):
        """Refill buffer from generator, restarting file if exhausted."""
        if self._exhausted or self._generator is None:
            self._generator = test_case_generator(self.filepath)
            self._exhausted = False
        
        # Fill buffer up to buffer_size
        try:
            new_items = list(islice(self._generator, self.buffer_size - len(self.buffer)))
            if not new_items:
                self._exhausted = True
                # Restart from beginning if we've exhausted the file
                self._generator = test_case_generator(self.filepath)
                new_items = list(islice(self._generator, self.buffer_size - len(self.buffer)))
            self.buffer.extend(new_items)
        except FileNotFoundError:
            pass  # File doesn't exist, buffer stays empty
    
    def get_random(self) -> dict:
        """Get a random test case from the buffer."""
        if not self.buffer:
            return None
        
        # Get random item and replace it with a new one from generator
        idx = random.randrange(len(self.buffer))
        item = self.buffer[idx]
        
        # Try to get a replacement from the generator
        try:
            replacement = next(self._generator)
            self.buffer[idx] = replacement
        except (StopIteration, TypeError):
            self._exhausted = True
            # Optionally restart the generator
            self._generator = test_case_generator(self.filepath)
        
        return item


# Shared buffer across all users (loaded once)
_shared_buffer = None


def get_shared_buffer() -> TestCaseBuffer:
    """Get or create the shared test case buffer."""
    global _shared_buffer
    if _shared_buffer is None:
        test_file = os.path.join(os.path.dirname(__file__), TEST_DATA_FILE)
        if os.path.exists(test_file):
            _shared_buffer = TestCaseBuffer(test_file, BUFFER_SIZE)
    return _shared_buffer


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
        # Use shared buffer if available, otherwise fall back to defaults
        self._buffer = get_shared_buffer()
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
    
    def _get_random_test_case(self) -> dict:
        """Get a random test case from the buffer or fallback."""
        if self._buffer:
            case = self._buffer.get_random()
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
        test_case = self._get_random_test_case()
        
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
        test_case = self._get_random_test_case()
        
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
