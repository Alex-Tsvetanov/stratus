# The shared service definition.
#
# This object is the whole interface. Both provider modules accept exactly this type and
# nothing else, so the service is described once and the choice of provider is the value
# of a single variable. Anything a module needs that is not in here is, by construction,
# a provider detail and belongs inside that module.

variable "target_provider" {
  description = "Which provider module to instantiate: aws or azure."
  type        = string

  validation {
    condition     = contains(["aws", "azure"], var.target_provider)
    error_message = "target_provider must be aws or azure."
  }
}

variable "region" {
  description = "Provider region. The value is provider specific, the variable is not."
  type        = string
}

variable "service" {
  description = "The provider independent description of the Stratus service."

  type = object({
    name           = string
    image          = string
    container_port = number

    # CPU is given in Fargate CPU units, where 1024 units is one vCPU. The Azure module
    # divides by 1024 to get the fractional core count Container Apps expects. A single
    # unit was needed and this one at least has a defined conversion in both directions.
    cpu_units = number
    memory_mb = number

    min_replicas       = number
    max_replicas       = number
    target_utilisation = number

    env  = map(string)
    tags = map(string)
  })

  validation {
    condition     = var.service.min_replicas >= 1 && var.service.max_replicas >= var.service.min_replicas
    error_message = "min_replicas must be at least 1 and no greater than max_replicas."
  }

  validation {
    condition     = var.service.target_utilisation > 0 && var.service.target_utilisation <= 1
    error_message = "target_utilisation is a fraction in (0, 1]."
  }
}
