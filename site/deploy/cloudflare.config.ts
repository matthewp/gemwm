import { defineConfig } from "cf/config";

// gemwm.org: GemWM's website, Astro's static build in ../dist (see
// wrangler.config.ts), served by a Worker. `pnpm run deploy`, in the site's
// folder, builds and publishes it. This folder is kept apart from the Astro
// project so cf takes the build as plain files, as augur.gemwm.org's is.
export default defineConfig({
  worker: {
    name: "gemwm-site",
    compatibilityDate: "2026-10-01",
    assets: {
      htmlHandling: "auto-trailing-slash",
      notFoundHandling: "404-page",
    },
    domains: ["gemwm.org", "www.gemwm.org"],
  },
});
