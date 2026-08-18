# Azure module: Container Apps, with a storage account the revision reaches through a user
# assigned managed identity rather than through a connection string.
#
# NOT APPLIED. There is no subscription and terraform is not installed on the machine this
# was written on, so this configuration has never been run against a live subscription. It
# is written to the documented resource schemas and formatted, and that is the entire claim
# being made about it.

terraform {
  required_version = ">= 1.6"
  required_providers {
    azurerm = {
      source  = "hashicorp/azurerm"
      version = "~> 3.100"
    }
    random = {
      source  = "hashicorp/random"
      version = "~> 3.6"
    }
  }
}

variable "region" {
  type = string
}

variable "service" {
  type = object({
    name               = string
    image              = string
    container_port     = number
    cpu_units          = number
    memory_mb          = number
    min_replicas       = number
    max_replicas       = number
    target_utilisation = number
    env                = map(string)
    tags               = map(string)
  })
}

# Storage account names are globally unique, lowercase, no separators, at most 24
# characters. A suffix is generated rather than derived from the deployment name so that
# two deployments of the same name in different subscriptions do not collide.
resource "random_string" "storage_suffix" {
  length  = 8
  special = false
  upper   = false
}

locals {
  name = var.service.name

  # The shared interface carries CPU in Fargate units. Container Apps wants fractional
  # cores in steps of 0.25, and memory in gibibytes as a string.
  cpu_cores = var.service.cpu_units / 1024
  memory_gi = format("%.2fGi", var.service.memory_mb / 1024)

  storage_name = substr(
    "${replace(lower(var.service.name), "/[^a-z0-9]/", "")}${random_string.storage_suffix.result}",
    0, 24
  )

  tags = merge(var.service.tags, {
    "stratus-deployment" = var.service.name
    "stratus-managed-by" = "terraform"
  })
}

resource "azurerm_resource_group" "this" {
  name     = "${local.name}-rg"
  location = var.region
  tags     = local.tags
}

# --- Object storage ---------------------------------------------------------

resource "azurerm_storage_account" "data" {
  name                            = local.storage_name
  resource_group_name             = azurerm_resource_group.this.name
  location                        = azurerm_resource_group.this.location
  account_tier                    = "Standard"
  account_replication_type        = "LRS"
  min_tls_version                 = "TLS1_2"
  allow_nested_items_to_be_public = false
  # Keys exist whether or not they are used. Disabling them removes the possibility of a
  # long lived credential being copied out of the portal and pasted somewhere.
  shared_access_key_enabled = false
  tags                      = local.tags
}

resource "azurerm_storage_container" "data" {
  name                  = "work"
  storage_account_name  = azurerm_storage_account.data.name
  container_access_type = "private"
}

# --- Identity ---------------------------------------------------------------

resource "azurerm_user_assigned_identity" "workload" {
  name                = "${local.name}-identity"
  resource_group_name = azurerm_resource_group.this.name
  location            = azurerm_resource_group.this.location
  tags                = local.tags
}

resource "azurerm_role_assignment" "blob" {
  scope                = azurerm_storage_account.data.id
  role_definition_name = "Storage Blob Data Contributor"
  principal_id         = azurerm_user_assigned_identity.workload.principal_id
}

# --- Compute ----------------------------------------------------------------

resource "azurerm_log_analytics_workspace" "this" {
  name                = "${local.name}-logs"
  resource_group_name = azurerm_resource_group.this.name
  location            = azurerm_resource_group.this.location
  sku                 = "PerGB2018"
  retention_in_days   = 30
  tags                = local.tags
}

resource "azurerm_container_app_environment" "this" {
  name                       = "${local.name}-env"
  resource_group_name        = azurerm_resource_group.this.name
  location                   = azurerm_resource_group.this.location
  log_analytics_workspace_id = azurerm_log_analytics_workspace.this.id
  tags                       = local.tags
}

resource "azurerm_container_app" "this" {
  name                         = local.name
  resource_group_name          = azurerm_resource_group.this.name
  container_app_environment_id = azurerm_container_app_environment.this.id
  revision_mode                = "Single"
  tags                         = local.tags

  identity {
    type         = "UserAssigned"
    identity_ids = [azurerm_user_assigned_identity.workload.id]
  }

  template {
    min_replicas = var.service.min_replicas
    max_replicas = var.service.max_replicas

    container {
      name   = "worker"
      image  = var.service.image
      cpu    = local.cpu_cores
      memory = local.memory_gi

      dynamic "env" {
        for_each = var.service.env
        content {
          name  = env.key
          value = env.value
        }
      }

      env {
        name  = "STRATUS_PORT"
        value = tostring(var.service.container_port)
      }

      env {
        name  = "STRATUS_OBJECT_STORE"
        value = azurerm_storage_container.data.name
      }

      env {
        name  = "STRATUS_STORAGE_ACCOUNT"
        value = azurerm_storage_account.data.name
      }

      env {
        name  = "AZURE_CLIENT_ID"
        value = azurerm_user_assigned_identity.workload.client_id
      }

      liveness_probe {
        transport = "HTTP"
        port      = var.service.container_port
        path      = "/healthz"
      }

      readiness_probe {
        transport = "HTTP"
        port      = var.service.container_port
        path      = "/healthz"
      }
    }

    # The provider side equivalent of the policy in include/stratus/scaling.hpp, fed the
    # same target from the same variable. Container Apps expresses CPU tracking as a
    # percentage utilisation threshold rather than as a target to converge on, so the two
    # providers are asked for the same number but do not implement it identically. That
    # difference is a result of the comparison, not a defect in it.
    custom_scale_rule {
      name             = "cpu-utilisation"
      custom_rule_type = "cpu"

      metadata = {
        type  = "Utilization"
        value = tostring(floor(var.service.target_utilisation * 100))
      }
    }
  }

  ingress {
    external_enabled = true
    target_port      = var.service.container_port
    transport        = "http"

    traffic_weight {
      latest_revision = true
      percentage      = 100
    }
  }
}

output "ingress_url" {
  value = "https://${azurerm_container_app.this.ingress[0].fqdn}"
}

output "object_store" {
  value = azurerm_storage_container.data.name
}

output "platform_id" {
  value = azurerm_container_app_environment.this.name
}

output "workload_identity" {
  value = azurerm_user_assigned_identity.workload.name
}
