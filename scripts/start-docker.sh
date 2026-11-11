if test ! -f "./scripts/docker-compose.yml"
then
  # in CI
  echo "Please run from duckdb root."
  exit 1
fi

# cd into scripts where docker-compose file is.
cd scripts

# need to have this happen in the background
set -ex

docker compose kill
docker compose rm -f

# Remove named volumes
docker volume rm $(docker volume ls -q --filter name=scripts_) || true

#  clean bind-mounted data directory
rm -rf ../data/*

docker compose up --detach

# Wait until MinIO is ready to accept requests
echo "Waiting for MinIO to become ready..."
ready=0
for i in {1..30}; do
  if curl -s -o /dev/null http://127.0.0.1:9000/minio/health/ready; then
    echo "MinIO is ready"
    ready=1
    break
  fi
  echo "Still waiting for MinIO..."
  sleep 10
done
if [ "$ready" != 1 ]; then
  echo "MinIO did not become ready after 300s" >&2
  exit 1
fi
