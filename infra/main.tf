# Root configuration.
#
# Exactly one of the two modules is instantiated, chosen by target_provider. Both receive
# the same object and both return the same outputs, so switching providers is a change of
# one variable and nothing else. The count expressions are the only place in the whole
# configuration where the provider is named outside its own module.

module "aws" {
  count  = var.target_provider == "aws" ? 1 : 0
  source = "./modules/aws"

  region  = var.region
  service = var.service
}

module "azure" {
  count  = var.target_provider == "azure" ? 1 : 0
  source = "./modules/azure"

  region  = var.region
  service = var.service
}
