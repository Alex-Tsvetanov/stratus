# AWS module: ECS on Fargate behind an application load balancer, with an S3 bucket the
# task reaches through a task role rather than through a stored key.
#
# NOT APPLIED. There are no credentials for this account and terraform is not installed on
# the machine this was written on, so this configuration has never been run against a live
# account. It is written to the documented resource schemas and formatted, and that is the
# entire claim being made about it.

terraform {
  required_version = ">= 1.6"
  required_providers {
    aws = {
      source  = "hashicorp/aws"
      version = "~> 5.0"
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

data "aws_availability_zones" "available" {
  state = "available"
}

locals {
  name = var.service.name

  # Two availability zones, which is the minimum an application load balancer accepts.
  azs = slice(data.aws_availability_zones.available.names, 0, 2)

  tags = merge(var.service.tags, {
    "stratus:deployment" = var.service.name
    "stratus:managed-by" = "terraform"
  })
}

# --- Network ----------------------------------------------------------------
# Public subnets only. The task needs outbound access to pull the image, and a NAT gateway
# is a fixed hourly charge that would land in the cost comparison without being part of
# what is being compared.

resource "aws_vpc" "this" {
  cidr_block           = "10.42.0.0/16"
  enable_dns_support   = true
  enable_dns_hostnames = true
  tags                 = merge(local.tags, { Name = local.name })
}

resource "aws_internet_gateway" "this" {
  vpc_id = aws_vpc.this.id
  tags   = local.tags
}

resource "aws_subnet" "public" {
  count                   = length(local.azs)
  vpc_id                  = aws_vpc.this.id
  cidr_block              = cidrsubnet(aws_vpc.this.cidr_block, 8, count.index)
  availability_zone       = local.azs[count.index]
  map_public_ip_on_launch = true
  tags                    = merge(local.tags, { Name = "${local.name}-public-${count.index}" })
}

resource "aws_route_table" "public" {
  vpc_id = aws_vpc.this.id

  route {
    cidr_block = "0.0.0.0/0"
    gateway_id = aws_internet_gateway.this.id
  }

  tags = local.tags
}

resource "aws_route_table_association" "public" {
  count          = length(aws_subnet.public)
  subnet_id      = aws_subnet.public[count.index].id
  route_table_id = aws_route_table.public.id
}

resource "aws_security_group" "lb" {
  name        = "${local.name}-lb"
  description = "Ingress to the Stratus load balancer"
  vpc_id      = aws_vpc.this.id

  ingress {
    description = "HTTP from anywhere"
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = local.tags
}

resource "aws_security_group" "task" {
  name        = "${local.name}-task"
  description = "Stratus worker tasks, reachable only from the load balancer"
  vpc_id      = aws_vpc.this.id

  ingress {
    description     = "Worker port from the load balancer only"
    from_port       = var.service.container_port
    to_port         = var.service.container_port
    protocol        = "tcp"
    security_groups = [aws_security_group.lb.id]
  }

  egress {
    from_port   = 0
    to_port     = 0
    protocol    = "-1"
    cidr_blocks = ["0.0.0.0/0"]
  }

  tags = local.tags
}

# --- Object storage ---------------------------------------------------------

resource "aws_s3_bucket" "data" {
  bucket        = "${local.name}-data"
  force_destroy = true
  tags          = local.tags
}

resource "aws_s3_bucket_public_access_block" "data" {
  bucket                  = aws_s3_bucket.data.id
  block_public_acls       = true
  block_public_policy     = true
  ignore_public_acls      = true
  restrict_public_buckets = true
}

resource "aws_s3_bucket_server_side_encryption_configuration" "data" {
  bucket = aws_s3_bucket.data.id

  rule {
    apply_server_side_encryption_by_default {
      sse_algorithm = "AES256"
    }
  }
}

# --- Identity ---------------------------------------------------------------
# The task role is the AWS half of the no stored credential rule: the container receives a
# short lived token from the task metadata endpoint, and the only right that token carries
# is reading and writing objects in this one bucket.

data "aws_iam_policy_document" "task_assume" {
  statement {
    actions = ["sts:AssumeRole"]

    principals {
      type        = "Service"
      identifiers = ["ecs-tasks.amazonaws.com"]
    }
  }
}

resource "aws_iam_role" "task" {
  name               = "${local.name}-task"
  assume_role_policy = data.aws_iam_policy_document.task_assume.json
  tags               = local.tags
}

data "aws_iam_policy_document" "bucket_access" {
  statement {
    actions   = ["s3:GetObject", "s3:PutObject", "s3:DeleteObject"]
    resources = ["${aws_s3_bucket.data.arn}/*"]
  }

  statement {
    actions   = ["s3:ListBucket"]
    resources = [aws_s3_bucket.data.arn]
  }
}

resource "aws_iam_role_policy" "bucket_access" {
  name   = "${local.name}-bucket-access"
  role   = aws_iam_role.task.id
  policy = data.aws_iam_policy_document.bucket_access.json
}

resource "aws_iam_role" "execution" {
  name               = "${local.name}-execution"
  assume_role_policy = data.aws_iam_policy_document.task_assume.json
  tags               = local.tags
}

resource "aws_iam_role_policy_attachment" "execution" {
  role       = aws_iam_role.execution.name
  policy_arn = "arn:aws:iam::aws:policy/service-role/AmazonECSTaskExecutionRolePolicy"
}

# --- Compute ----------------------------------------------------------------

resource "aws_cloudwatch_log_group" "this" {
  name              = "/stratus/${local.name}"
  retention_in_days = 7
  tags              = local.tags
}

resource "aws_ecs_cluster" "this" {
  name = local.name
  tags = local.tags
}

resource "aws_ecs_task_definition" "this" {
  family                   = local.name
  requires_compatibilities = ["FARGATE"]
  network_mode             = "awsvpc"
  cpu                      = tostring(var.service.cpu_units)
  memory                   = tostring(var.service.memory_mb)
  execution_role_arn       = aws_iam_role.execution.arn
  task_role_arn            = aws_iam_role.task.arn
  tags                     = local.tags

  container_definitions = jsonencode([
    {
      name      = "worker"
      image     = var.service.image
      essential = true

      portMappings = [
        {
          containerPort = var.service.container_port
          protocol      = "tcp"
        }
      ]

      environment = concat(
        [for k, v in var.service.env : { name = k, value = v }],
        [
          { name = "STRATUS_PORT", value = tostring(var.service.container_port) },
          { name = "STRATUS_OBJECT_STORE", value = aws_s3_bucket.data.bucket },
        ]
      )

      logConfiguration = {
        logDriver = "awslogs"
        options = {
          "awslogs-group"         = aws_cloudwatch_log_group.this.name
          "awslogs-region"        = var.region
          "awslogs-stream-prefix" = "worker"
        }
      }
    }
  ])
}

resource "aws_lb" "this" {
  name               = substr("${local.name}-lb", 0, 32)
  load_balancer_type = "application"
  subnets            = aws_subnet.public[*].id
  security_groups    = [aws_security_group.lb.id]
  tags               = local.tags
}

resource "aws_lb_target_group" "this" {
  name        = substr("${local.name}-tg", 0, 32)
  port        = var.service.container_port
  protocol    = "HTTP"
  target_type = "ip"
  vpc_id      = aws_vpc.this.id

  health_check {
    path                = "/healthz"
    matcher             = "200"
    interval            = 10
    healthy_threshold   = 2
    unhealthy_threshold = 3
  }

  tags = local.tags
}

resource "aws_lb_listener" "http" {
  load_balancer_arn = aws_lb.this.arn
  port              = 80
  protocol          = "HTTP"

  default_action {
    type             = "forward"
    target_group_arn = aws_lb_target_group.this.arn
  }
}

resource "aws_ecs_service" "this" {
  name            = local.name
  cluster         = aws_ecs_cluster.this.id
  task_definition = aws_ecs_task_definition.this.arn
  desired_count   = var.service.min_replicas
  launch_type     = "FARGATE"

  network_configuration {
    subnets          = aws_subnet.public[*].id
    security_groups  = [aws_security_group.task.id]
    assign_public_ip = true
  }

  load_balancer {
    target_group_arn = aws_lb_target_group.this.arn
    container_name   = "worker"
    container_port   = var.service.container_port
  }

  # desired_count is owned by the autoscaler once it exists. Without this the next plan
  # would helpfully undo every scaling decision the controller made.
  lifecycle {
    ignore_changes = [desired_count]
  }

  depends_on = [aws_lb_listener.http]
}

# --- Autoscaling ------------------------------------------------------------
# Target tracking on average CPU, which is the provider's own equivalent of the policy
# implemented in include/stratus/scaling.hpp. The thresholds come from the same variable,
# so the two deployments and the local stack are asked for the same thing.

resource "aws_appautoscaling_target" "this" {
  service_namespace  = "ecs"
  resource_id        = "service/${aws_ecs_cluster.this.name}/${aws_ecs_service.this.name}"
  scalable_dimension = "ecs:service:DesiredCount"
  min_capacity       = var.service.min_replicas
  max_capacity       = var.service.max_replicas
}

resource "aws_appautoscaling_policy" "cpu" {
  name               = "${local.name}-target-tracking"
  policy_type        = "TargetTrackingScaling"
  service_namespace  = aws_appautoscaling_target.this.service_namespace
  resource_id        = aws_appautoscaling_target.this.resource_id
  scalable_dimension = aws_appautoscaling_target.this.scalable_dimension

  target_tracking_scaling_policy_configuration {
    target_value       = var.service.target_utilisation * 100
    scale_in_cooldown  = 60
    scale_out_cooldown = 30

    predefined_metric_specification {
      predefined_metric_type = "ECSServiceAverageCPUUtilization"
    }
  }
}

output "ingress_url" {
  value = "http://${aws_lb.this.dns_name}"
}

output "object_store" {
  value = aws_s3_bucket.data.bucket
}

output "platform_id" {
  value = aws_ecs_cluster.this.name
}

output "workload_identity" {
  value = aws_iam_role.task.name
}
