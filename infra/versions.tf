terraform {
  required_version = ">= 1.6"

  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
    }
    azurerm = {
      source  = "hashicorp/azurerm"
      version = "~> 3.100"
    }
  }
}

# Both providers are configured unconditionally, because Terraform evaluates provider
# blocks regardless of whether any resource uses them. Neither authenticates until a
# resource is actually planned, so configuring the unused one costs nothing.

provider "aws" {
  region = var.target_provider == "aws" ? var.region : "us-east-1"
}

provider "azurerm" {
  features {}
  # subscription_id comes from the ARM_SUBSCRIPTION_ID environment variable or from
  # `az login`. It is never written here, and never committed.
}
