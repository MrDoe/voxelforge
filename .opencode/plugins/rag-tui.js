// Wrapper for the V1-era TUI plugin under OpenCode V2.
//
// OpenCode 2.x and @opentui/core share one global env registry
// (globalThis[Symbol.for("@opentui/core/singleton")]["env-registry"]). The
// bundled opentui registers OPENTUI_* presence flags as
// `{ type: "string", required: false, description: "... when the variable is
// present" }`; after the plugin module is imported, the OpenCode loader
// re-registers the same flags in the current boolean form
// (`{ type: "boolean", default: false }` with the suffix-stripped
// description). The strict compare against the legacy entry then throws and
// aborts this plugin's load with:
//   Environment variable "OPENTUI_FORCE_WCWIDTH" is already registered with
//   different configuration...
// Normalize presence-style entries to the boolean form once @opentui/core has
// finished its import side effects (import statements above run before this
// body), so the loader's re-registration compares equal. The boolean form is
// upstream's current documented schema for these flags.
import plugin from "../node_modules/opencode-rag-plugin/dist/tui.js";

try {
  const registry = globalThis[Symbol.for("@opentui/core/singleton")]?.["env-registry"];
  if (registry) {
    const SUFFIX = " when the variable is present";
    for (const [name, config] of Object.entries(registry)) {
      if (
        config &&
        config.type === "string" &&
        config.required === false &&
        typeof config.description === "string" &&
        config.description.endsWith(SUFFIX)
      ) {
        registry[name] = {
          name,
          description: config.description.slice(0, -SUFFIX.length),
          type: "boolean",
          default: false,
        };
      }
    }
  }
} catch {
  // Best-effort registry reconciliation; never block the plugin load.
}

export default plugin;
