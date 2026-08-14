"""Versioned customer data API for the ATX warehouse.

The neutral catalog/auth/query modules remain importable without the optional
``server`` dependencies.  Import ``atx_db.api.app`` only when serving HTTP.
"""

from .auth import ApiPrincipal, Authenticator, DuckDBApiKeyAuthenticator, StaticApiKeyAuthenticator
from .batch import (
    BatchJob,
    BatchJobRepository,
    DuckDBBatchJobRepository,
    InMemoryBatchJobRepository,
    LocalBatchManager,
)
from .catalog import DATASETS, DatasetSpec, FieldSpec, RecordSchema, get_dataset, get_schema
from .commercial import (
    DuckDBIdempotencyStore,
    DuckDBPricingCatalog,
    IdempotencyStore,
    InMemoryIdempotencyStore,
    InMemoryPricingCatalog,
    InMemoryRateLimiter,
    PricingCatalog,
    RateLimiter,
    UnitPrice,
)
from .usage import DuckDBUsageLedger, InMemoryUsageLedger, UsageEvent, UsageLedger

__all__ = [
    "DATASETS",
    "ApiPrincipal",
    "Authenticator",
    "BatchJob",
    "BatchJobRepository",
    "DatasetSpec",
    "DuckDBApiKeyAuthenticator",
    "DuckDBBatchJobRepository",
    "DuckDBIdempotencyStore",
    "DuckDBPricingCatalog",
    "DuckDBUsageLedger",
    "FieldSpec",
    "IdempotencyStore",
    "InMemoryBatchJobRepository",
    "InMemoryIdempotencyStore",
    "InMemoryPricingCatalog",
    "InMemoryRateLimiter",
    "InMemoryUsageLedger",
    "LocalBatchManager",
    "PricingCatalog",
    "RateLimiter",
    "RecordSchema",
    "StaticApiKeyAuthenticator",
    "UnitPrice",
    "UsageEvent",
    "UsageLedger",
    "get_dataset",
    "get_schema",
]
