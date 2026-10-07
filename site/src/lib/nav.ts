import { getCollection, type CollectionEntry } from 'astro:content';

export const SECTIONS = ['Getting Started', 'Customizing', 'Modes', 'Going Further'] as const;

export const REPO = 'https://github.com/matthewp/gemwm';

export async function docs() {
	const all = await getCollection('docs');
	return all.sort((a, b) =>
		SECTIONS.indexOf(a.data.section) - SECTIONS.indexOf(b.data.section) ||
		a.data.order - b.data.order);
}

export async function docSections() {
	const all = await docs();
	return SECTIONS.map((section) => ({
		section,
		pages: all.filter((d) => d.data.section === section),
	}));
}

export async function apps() {
	const all = await getCollection('apps');
	return all.sort((a, b) =>
		(a.data.kind === b.data.kind ? 0 : a.data.kind === 'app' ? -1 : 1) ||
		a.data.order - b.data.order);
}

export const docHref = (d: CollectionEntry<'docs'>) => `/docs/${d.id}/`;
export const appHref = (a: CollectionEntry<'apps'>) => `/apps/${a.id}/`;
