'use strict';
'require baseclass';
'require ui';

/*
 * Slate navigation: the menu tree as sidebar groups, the active page's
 * tabs, breadcrumbs, the mobile drawer and the colour scheme switch.
 * Menu lookups go through ui.menu so LuCI's ACL filtering, ordering and
 * aliases apply.
 */

return baseclass.extend({
	__init__() {
		this.setupShell();
		ui.menu.load().then((tree) => this.render(tree));

		/* Save & Apply in edit dialogs and a bar for staged changes. */
		if (L.env.sessionid)
			L.require('slate-changes').catch(() => {});
	},

	render(tree) {
		const path = L.env.dispatchpath;
		let node = tree;
		let url = '';

		this.renderModeMenu(tree);

		/* PON line state in the top bar, where luci-app-xpon is installed. */
		if (tree.children?.admin?.children?.network?.children?.xpon)
			L.require('xpon.indicator').catch(() => {});

		if (path.length < 3)
			return;

		for (let i = 0; i < 3 && node; i++) {
			node = ui.menu.getChildren(node).find(child => child.name === path[i]) ?? node.children?.[path[i]];
			url += (url ? '/' : '') + path[i];
		}

		if (node)
			this.renderTabMenu(node, url, 0);
	},

	renderTabMenu(tree, url, level) {
		const container = document.querySelector('#tabmenu');
		const ul = E('ul', { 'class': 'tabs' });
		let active = null;

		for (const child of ui.menu.getChildren(tree)) {
			const isActive = (L.env.dispatchpath[3 + level] == child.name);

			ul.appendChild(E('li', { 'class': 'tabmenu-item-' + child.name + (isActive ? ' active' : '') }, [
				E('a', { 'href': L.url(url, child.name) }, [ _(child.title) ])
			]));

			if (isActive)
				active = child;
		}

		if (!ul.children.length)
			return;

		container.appendChild(ul);
		container.style.display = '';

		if (active)
			this.renderTabMenu(active, url + '/' + active.name, level + 1);
	},

	/* One group per category; the current one starts open. */
	renderMainMenu(tree, url) {
		const ul = document.querySelector('#topmenu');
		const path = L.env.dispatchpath;

		for (const child of ui.menu.getChildren(tree)) {
			const items = ui.menu.getChildren(child);
			const current = child.name == path[1];
			const icon = E('span', { 'class': 'nav-icon', 'data-icon': child.name, 'aria-hidden': 'true' });
			const li = E('li', { 'class': 'nav-group' + (current ? ' active' : '') });

			if (items.length) {
				const sub = E('ul', { 'class': 'nav-sub' });
				const head = E('button', {
					'type': 'button',
					'class': 'nav-head',
					'aria-expanded': current ? 'true' : 'false',
					'click': () => {
						const open = head.getAttribute('aria-expanded') != 'true';

						head.setAttribute('aria-expanded', open ? 'true' : 'false');
						li.classList.toggle('open', open);
					}
				}, [ icon, E('span', { 'class': 'nav-label' }, [ _(child.title) ]), E('span', { 'class': 'nav-chevron', 'aria-hidden': 'true' }) ]);

				for (const item of items) {
					const here = current && item.name == path[2];

					sub.appendChild(E('li', { 'class': here ? 'active' : '' }, [
						E('a', { 'href': L.url(url, child.name, item.name), 'aria-current': here ? 'page' : null }, [ _(item.title) ])
					]));

					if (here)
						this.setCrumbs(child.title, item.title);
				}

				if (current)
					li.classList.add('open');
				li.appendChild(head);
				li.appendChild(sub);
			}
			else {
				li.classList.add('nav-leaf');
				li.appendChild(E('a', { 'class': 'nav-head', 'href': L.url(url, child.name) }, [
					icon, E('span', { 'class': 'nav-label' }, [ _(child.title) ])
				]));

				if (current)
					this.setCrumbs(child.title);
			}

			ul.appendChild(li);
		}

		ul.style.display = '';
	},

	renderModeMenu(tree) {
		const ul = document.querySelector('#modemenu');
		const children = ui.menu.getChildren(tree);

		children.forEach((child, index) => {
			const isActive = L.env.requestpath.length
				? child.name === L.env.requestpath[0]
				: index === 0;

			ul.appendChild(E('li', { 'class': isActive ? 'active' : '' }, [
				E('a', { 'href': L.url(child.name) }, [ _(child.title) ])
			]));

			if (isActive)
				this.renderMainMenu(child, child.name);
		});

		if (ul.children.length > 1)
			ul.style.display = '';
	},

	setCrumbs(section, page) {
		const crumbs = document.querySelector('#crumbs');

		if (!crumbs)
			return;

		crumbs.appendChild(E('span', { 'class': 'crumb-section' }, [ _(section) ]));
		if (page)
			crumbs.appendChild(E('span', { 'class': 'crumb-page' }, [ _(page) ]));
	},

	/* Drawer and scheme switch; both live in the server-rendered shell. */
	setupShell() {
		const body = document.body;
		const toggle = document.querySelector('#menu-toggle');
		const scrim = document.querySelector('#scrim');
		const setOpen = (open) => {
			body.classList.toggle('nav-open', open);
			toggle?.setAttribute('aria-expanded', open ? 'true' : 'false');
		};

		toggle?.addEventListener('click', () => setOpen(!body.classList.contains('nav-open')));
		scrim?.addEventListener('click', () => setOpen(false));
		document.addEventListener('keydown', (ev) => {
			if (ev.key == 'Escape' && body.classList.contains('nav-open'))
				setOpen(false);
		});

		document.querySelectorAll('.scheme button').forEach((button) => {
			button.addEventListener('click', () => window.slateScheme?.(button.dataset.scheme));
		});
	}
});
