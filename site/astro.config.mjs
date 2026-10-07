// @ts-check
import { defineConfig } from 'astro/config';

export default defineConfig({
	site: 'https://gemwm.org',
	trailingSlash: 'always',
	markdown: {
		// Code is black on white, in the ST font, as GemWM's terminal shows it.
		syntaxHighlight: false,
	},
});
