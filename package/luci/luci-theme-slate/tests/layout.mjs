// SPDX-License-Identifier: Apache-2.0
import { createRequire } from 'node:module';
import { createServer } from 'node:http';
import { readFile } from 'node:fs/promises';
import { dirname, resolve, extname, sep } from 'node:path';
import { fileURLToPath } from 'node:url';

const require = createRequire(import.meta.url);
const { chromium } = require(process.env.PLAYWRIGHT_MODULE || 'playwright');
const repo = resolve(dirname(fileURLToPath(import.meta.url)), '../../../..');
const loginTemplate = await readFile(resolve(repo, 'package/luci/luci-theme-slate/ucode/template/themes/slate/sysauth.ut'), 'utf8');
const loginScript = loginTemplate.match(/<script>\s*([\s\S]*?)<\/script>/)[1];
const base = '/package/luci/luci-theme-slate/tests/layout.html';
const types = { '.html': 'text/html', '.css': 'text/css', '.js': 'text/javascript' };
const server = createServer(async (req, res) => {
	const path = resolve(repo, '.' + new URL(req.url, 'http://localhost').pathname);
	if (!path.startsWith(repo + sep)) { res.writeHead(403).end(); return; }
	try {
		res.setHeader('Content-Type', types[extname(path)] || 'text/plain');
		res.end(await readFile(path));
	}
	catch { res.writeHead(404).end(); }
});
await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
let browser;
const failures = [];
const widths = (process.env.WIDTHS || '320,375,600,800,801,1024,1440').split(',').map(Number);
const scenarios = (process.env.SCENARIOS || 'form,table,interfaces,modal,login,overview,status,narrow,dropdown,epon').split(',');
try {
	browser = await chromium.launch({ executablePath: process.env.CHROME || '/usr/bin/google-chrome', args: ['--no-sandbox'] });
	for (const width of widths) {
		for (const dark of [false, true]) {
			const page = await browser.newPage({ viewport: { width, height: 900 }, hasTouch: width <= 800 });
			for (const scenario of scenarios) {
				const errors = [];
				const onError = e => errors.push(e.message);
				page.on('pageerror', onError);
				await page.goto(`http://127.0.0.1:${server.address().port}${base}?case=${scenario}&dark=${dark}`);
				await page.evaluate(() => window.ready);
				if (scenario === 'login') {
					await page.addScriptTag({content: loginScript});
					await page.evaluate(() => document.querySelector('form').addEventListener('submit', ev => ev.preventDefault()));
					await page.getByRole('button', {name: 'Log in', exact: true}).click();
					if (await page.getByRole('button', {name: 'Log in', exact: true}).isDisabled())
						errors.push('cancelled login submission disables retry');
				}
				const navLink = page.locator('#topmenu > li:last-child > a');
				if (await navLink.count()) await navLink.focus();
				await page.evaluate(() => new Promise(requestAnimationFrame));
				const problems = await page.evaluate(() => {
					const errors = [];
					const vw = document.documentElement.clientWidth;
					if (document.documentElement.scrollWidth > vw + 1)
						errors.push(`page overflow ${document.documentElement.scrollWidth} > ${vw}`);
					for (const button of document.querySelectorAll('[data-no-word-wrap]')) {
						const range = document.createRange();
						range.selectNodeContents(button);
						if (range.getClientRects().length > 1)
							errors.push(`action button splits a word: ${button.textContent}`);
					}
					for (const container of document.querySelectorAll('[data-bounds]')) {
						const outer = container.getBoundingClientRect();
						for (const child of container.children) {
							const style = getComputedStyle(child), rect = child.getBoundingClientRect();
							if (!rect.width || style.position === 'absolute' || style.position === 'fixed') continue;
							if (rect.left < outer.left - 1 || rect.right > outer.right + 1)
								errors.push(`${container.className || container.tagName} contains overflowing ${child.tagName}.${child.className}`);
						}
					}
					const grid = document.querySelector('#custom-grid');
					if (grid && getComputedStyle(grid).gridTemplateColumns.split(' ').length !== 2)
						errors.push('port styling overrides unrelated status grid');
					const popup = document.querySelector('#topmenu > li:last-child .dropdown-menu');
					if (popup) {
						const rect = popup.getBoundingClientRect();
						if (rect.left < -1 || rect.right > vw + 1)
							errors.push(`navigation popup outside viewport: ${rect.left}, ${rect.right}`);
					}
					const rich = document.querySelector('#rich-dropdown > ul.dropdown');
					if (rich) {
						const rect = rich.getBoundingClientRect();
						if (rect.left < -1 || rect.right > vw + 1 || rich.scrollWidth > rich.clientWidth + 1)
							errors.push(`rich dropdown clips or overflows: ${rect.left}, ${rect.right}`);
					}
					return errors;
				});
				if (problems.length || errors.length)
					failures.push({ width, dark, scenario, problems, errors });
				if (scenario === 'modal') {
					const actions = page.locator('.modal > .button-row');
					await actions.scrollIntoViewIfNeeded();
					const rect = await actions.boundingBox();
					if (!rect || rect.y < -1 || rect.y + rect.height > 901)
						failures.push({width, dark, scenario, problems: ['modal actions cannot be scrolled into view']});
				}
				if (problems.length && process.env.DEBUG_LAYOUT)
					console.error(await page.evaluate(() => Array.from(document.querySelectorAll('main *')).filter(el =>
						el.getBoundingClientRect().right > innerWidth + 1).map(el => ({
							tag: el.tagName, class: el.className, width: el.getBoundingClientRect().width,
							display: getComputedStyle(el).display, min: getComputedStyle(el).minWidth,
							max: getComputedStyle(el).maxWidth, text: el.textContent.slice(0, 45)
						})).slice(0, 20)));
				if (process.env.SCREENSHOT_DIR && [320, 1440].includes(width) && !dark && ['form', 'table', 'interfaces', 'modal', 'login', 'status'].includes(scenario))
					{
						await page.evaluate(() => document.activeElement?.blur());
						await page.screenshot({ path: `${process.env.SCREENSHOT_DIR}/${scenario}-${width}.png`, fullPage: true });
					}
				page.off('pageerror', onError);
			}
			await page.goto(`http://127.0.0.1:${server.address().port}/package/luci/luci-theme-slate/tests/visibility.html`);
			if (await page.locator('body').getAttribute('data-result') !== 'pass')
				failures.push({width, dark, scenario: 'visibility', problems: [await page.locator('#result').textContent()]});
			await page.close();
		}
	}
}
finally {
	await browser?.close();
	await new Promise(resolve => server.close(resolve));
}
console.log(JSON.stringify({ cases: widths.length * 2 * scenarios.length, visibilityCases: widths.length * 2, failures }, null, 2));
process.exitCode = failures.length ? 1 : 0;
