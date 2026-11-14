"""FengquDB-backed, incremental and provider-independent LLM Wiki."""

from .wiki import Wiki, WikiError
from .limits import Limits, BudgetError, ConflictError
from .index_provider import IndexDefinition, IndexProvider
from .native_index_provider import NativeIndexProvider

__all__ = ["Wiki", "WikiError", "Limits", "BudgetError", "ConflictError", "IndexDefinition", "IndexProvider", "NativeIndexProvider"]
__version__ = "0.2.0"
