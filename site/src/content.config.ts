import { defineCollection } from 'astro:content';
import { glob } from 'astro/loaders';
import { z } from 'astro/zod';

/* The manual, in sections down the side of the Docs window. */
const docs = defineCollection({
	loader: glob({ pattern: '**/*.md', base: './src/content/docs' }),
	schema: z.object({
		title: z.string(),
		description: z.string(),
		section: z.enum(['Getting Started', 'Customizing', 'Modes', 'Going Further']),
		order: z.number(),
		/* Shorter name for the side and the menu, if the title is long. */
		short: z.string().optional(),
	}),
});

/* GemWM's applications and the small utilities around the menu bar. */
const apps = defineCollection({
	loader: glob({ pattern: '**/*.md', base: './src/content/apps' }),
	schema: z.object({
		title: z.string(),
		description: z.string(),
		kind: z.enum(['app', 'utility']),
		icon: z.string(),
		command: z.string(),
		order: z.number(),
		screenshot: z.string().optional(),
		alt: z.string().optional(),
	}),
});

export const collections = { docs, apps };
