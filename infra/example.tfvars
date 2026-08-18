# Copy, fill in and keep out of version control. Every *.tfvars file except this one is
# gitignored. Nothing here is an account identifier, a subscription id or a key.

target_provider = "aws" # or "azure"
region          = "<provider-region>"

service = {
  name           = "stratus-dev"
  image          = "<registry>/<repository>:<tag>"
  container_port = 8081

  # 1024 CPU units is one vCPU. The Azure module divides by 1024 for Container Apps.
  cpu_units = 1024
  memory_mb = 2048

  min_replicas       = 1
  max_replicas       = 6
  target_utilisation = 0.70

  env = {
    STRATUS_THREADS      = "1"
    STRATUS_DEFAULT_SIZE = "384"
    STRATUS_DEFAULT_ITER = "500"
  }

  tags = {
    project = "stratus"
  }
}
