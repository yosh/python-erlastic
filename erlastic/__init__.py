
"""Erlang External Term Format serializer/deserializer"""

__version__ = "1.0.0"
__license__ = "BSD"

from erlastic.types import *

from erlastic.codec import ErlangTermDecoder as PythonErlangTermDecoder
from erlastic.codec import ErlangTermEncoder as PythonErlangTermEncoder

try:
    from _erlastic import ErlangTermDecoder, ErlangTermEncoder
except ImportError:
    ErlangTermDecoder = PythonErlangTermDecoder
    ErlangTermEncoder = PythonErlangTermEncoder

encode = ErlangTermEncoder().encode
decode = ErlangTermDecoder().decode
