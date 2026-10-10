// @ts-check
import { defineConfig } from 'astro/config';

export default defineConfig({
	site: 'https://gemwm.org',
	trailingSlash: 'always',
	// Every link fetched when the pointer rests on it, so the click lands
	// at once: 35 small pages, and the menus are mostly links.
	prefetch: {
		prefetchAll: true,
		defaultStrategy: 'hover',
	},
	markdown: {
		// Code is black on white, in the ST font, as GemWM's terminal shows it.
		syntaxHighlight: false,
	},
});
