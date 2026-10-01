"""Where API keys come from. They are never written to the profiles file.

Order: the configured environment variable, the system keyring (optional), then a key typed at the prompt (memory
only).
"""

import os
from dataclasses import dataclass

KEYRING_SERVICE = "mcpchat"


def llm_key_account(profile_name):
    return f"llm:{profile_name}"


def server_token_account(server_name):
    return f"mcp:{server_name}"


@dataclass
class ResolvedSecret:
    value: str
    source: str


def _load_keyring():
    try:
        import keyring
    except ImportError:
        return None
    return keyring


class SecretStore:
    def __init__(self, environ=None, keyring_loader=_load_keyring):
        self._environ = os.environ if environ is None else environ
        self._keyring_loader = keyring_loader
        self._memory = {}

    def resolve(self, account, env_var=""):
        value = self._environ.get(env_var, "") if env_var else ""
        if value:
            return ResolvedSecret(value, "env")
        value = self._from_keyring(account)
        if value:
            return ResolvedSecret(value, "keyring")
        value = self._memory.get(account, "")
        if value:
            return ResolvedSecret(value, "memory")
        return ResolvedSecret("", "none")

    def set_memory(self, account, value):
        if value:
            self._memory[account] = value
        else:
            self._memory.pop(account, None)

    def keyring_available(self):
        return self._keyring_loader() is not None

    def save_to_keyring(self, account, value):
        keyring = self._keyring_loader()
        if keyring is None:
            return False
        try:
            keyring.set_password(KEYRING_SERVICE, account, value)
        except Exception:  # keyring backends raise their own error types
            return False
        return True

    def _from_keyring(self, account):
        keyring = self._keyring_loader()
        if keyring is None:
            return ""
        try:
            return keyring.get_password(KEYRING_SERVICE, account) or ""
        except Exception:  # no usable backend (headless Linux, locked keychain...)
            return ""
