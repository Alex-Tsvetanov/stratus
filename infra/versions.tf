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
# blocks regardless of whether any resource uses them.
#
# The unused one is not free, which is only visible once the plan is actually run. The
# AWS provider resolves credentials while the provider is configured, not when a resource
# is planned, so `terraform plan` for the Azure target failed on missing AWS credentials
# after having already computed the entire Azure plan. It even reached out to the EC2
# instance metadata endpoint at 169.254.169.254 and waited for that to time out.
#
# The three skips below are the supported way to configure the provider without
# credentials. They only take effect for the provider that is not being used: when the
# target really is AWS, every one of these checks is done by the resources themselves.
provider "aws" {
  region = var.target_provider == "aws" ? var.region : "us-east-1"

  skip_credentials_validation = var.target_provider != "aws"
  skip_requesting_account_id  = var.target_provider != "aws"
  skip_metadata_api_check     = var.target_provider != "aws"

  # The skips alone are not enough: the provider still insists on finding a credential
  # of some kind before it will configure. These two are literal placeholders, used only
  # on the path where no AWS resource is planned. They are not secrets, they are not an
  # account, and when target_provider is aws they are null and the real credential chain
  # applies unchanged.
  access_key = var.target_provider == "aws" ? null : "placeholder"
  secret_key = var.target_provider == "aws" ? null : "placeholder"
}

provider "azurerm" {
  features {}

  # The data plane is reached with an Entra ID token rather than a shared account key.
  # Not a preference: the tenant this ran in forbids key based authentication on storage
  # accounts outright, and the provider's default is to poll the blob endpoint with a key,
  # so apply failed with KeyBasedAuthenticationNotPermitted after creating the account.
  storage_use_azuread = true
  # subscription_id comes from the ARM_SUBSCRIPTION_ID environment variable or from
  # `az login`. It is never written here, and never committed.
}
