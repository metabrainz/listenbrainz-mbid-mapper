# listenbrainz-mbid-mapper

MBID mapping service for mapping ListenBrainz music metadata to MusicBrainz IDs.

## Quick Start

### 0. Prerequisites

To work with this project and build the mapping you'll need musicbrainz-docker installed and the full DB loaded:

https://github.com/metabrainz/musicbrainz-docker

Then you'll need the listenbrainz-server project:

https://github.com/metabrainz/listenbrainz-server

Then you'll need to create the local canonical data. Make sure your MB database has the "mapping" schema.

```bash
cd mbid_mapper
./manage.py canonical-data --use-mb-conn
```

### 1. Clone repository

```bash
git clone --recurse-submodules https://github.com/metabrainz/listenbrainz-mbid-mapper.git
cd listenbrainz-mbid-mapper
```

### 2. Setup

If you're going to use a local development environment use .env-local-dev as a starting point: (this is recommended for development)

```bash
cp .env-local-dev .env
```

You'll also need to install libboost-dev and libreadline-dev for local development.

For deployment or running it in docker:

```bash
cp .env-docker .env
```

Then edit .env to reflect your requirements.

### 3. Build

```bash
./build-debug.sh
```

Or for a production optimized version:

```bash
./build-release.sh
```

### 3. Create Base Mapping

```bash
./build/make_mapping
```

### 4. Build Indexes

```bash
./build/make_indexes
```

### 5. Run Server

```bash
./build/server
```

Then open http://localhost:5000 in your browser. See deceptively simple this setup is? You'd be crazy to try it.
