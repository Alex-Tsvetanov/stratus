# The outputs are the second half of the shared interface. The load generator and the
# controller are pointed at ingress_url and never learn which provider answered.

output "ingress_url" {
  description = "Base URL of the deployed service."
  value = var.target_provider == "aws" ? one(module.aws[*].ingress_url) : one(module.azure[*].ingress_url)
}

output "object_store" {
  description = "Name of the object store created for the deployment."
  value = var.target_provider == "aws" ? one(module.aws[*].object_store) : one(module.azure[*].object_store)
}

output "platform_id" {
  description = "Identifier of the compute platform holding the service."
  value = var.target_provider == "aws" ? one(module.aws[*].platform_id) : one(module.azure[*].platform_id)
}

output "workload_identity" {
  description = "Name of the identity the workload assumes to reach the object store."
  value = var.target_provider == "aws" ? one(module.aws[*].workload_identity) : one(module.azure[*].workload_identity)
}
