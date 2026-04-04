"""Base class for menu plugins. Drop a new .py file in plugins/ to add a menu entry."""

from abc import ABC, abstractmethod


class PluginBase(ABC):
    """Subclass this to create a new menu plugin.

    Attributes:
        name: Display name shown in the main menu.
        order: Sort order in the menu (lower = higher). Default 50.
    """

    name = "Unnamed"
    order = 50

    def __init__(self, ctx):
        """
        Args:
            ctx: dict with 'display', 'config' keys.
        """
        self.ctx = ctx
        self.display = ctx["display"]
        self.config = ctx["config"]

    def on_enter(self):
        """Called when the plugin becomes active."""
        pass

    def on_exit(self):
        """Called when leaving the plugin."""
        pass

    @abstractmethod
    def handle_key(self, key):
        """Handle a key press.

        Args:
            key: one of UP, DOWN, LEFT, RIGHT, ENTER, BACK

        Returns:
            False to exit back to the parent menu, True to stay active.
        """
        pass
