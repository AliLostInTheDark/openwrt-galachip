'use strict';
'require baseclass';
'require ui';

/*
 * Applying staged changes from where the user is.
 *
 * LuCI's edit dialogs only stage a change: their "Save" keeps it in the
 * session until the page's "Save & Apply" runs. Next to that "Save" this adds
 * "Save & Apply", which saves the dialog and then applies, exactly as the
 * page's own button does. While changes are staged and the page's own
 * "Save & Apply" is out of view (or the page has none), a bar at the bottom
 * offers Review, Revert and Save & Apply.
 *
 * Only ui.changes.setIndicator, ui.showModal and ui.hideModal are wrapped;
 * LuCI's own behaviour is unchanged.
 */

return baseclass.extend({
	__init__() {
		this.count = 0;
		this.pendingApply = false;
		this.pageApplyVisible = false;
		this.bar = null;
		this.observed = null;
		this.hookIndicator();
		this.hookModals();
	},

	/* Staged changes in an object shaped like ui.changes.changes: { config: [change, ...] } */
	countChanges(changes) {
		let n = 0;

		for (const config in changes ?? {})
			n += Array.isArray(changes[config]) ? changes[config].length : 0;

		return n;
	},

	label(n) {
		return n == 1 ? _('1 unsaved change') : _('%d unsaved changes').format(n);
	},

	hookIndicator() {
		const changes = ui.changes;

		if (!changes || typeof changes.setIndicator != 'function')
			return;

		const orig = changes.setIndicator;
		const self = this;

		changes.setIndicator = function(n) {
			const rv = orig.apply(this, arguments);
			self.update(n);
			return rv;
		};

		this.update(this.countChanges(changes.changes));
	},

	hookModals() {
		const show = ui.showModal, hide = ui.hideModal;
		const self = this;

		if (typeof show != 'function' || typeof hide != 'function')
			return;

		ui.showModal = function() {
			self.pendingApply = false;
			const dlg = show.apply(this, arguments);

			try { self.enhanceModal(dlg); } catch (e) { /* never break a dialog */ }

			return dlg;
		};

		ui.hideModal = function() {
			const rv = hide.apply(this, arguments);

			if (self.pendingApply) {
				self.pendingApply = false;
				/* let the dialog's save handler finish staging first */
				window.setTimeout(() => self.applyNow(), 0);
			}

			return rv;
		};
	},

	/* The positive "Save" button of a form dialog, or null. */
	findModalSave(dlg) {
		if (!dlg?.classList?.contains('cbi-modal'))
			return null;

		return Array.prototype.find.call(dlg.querySelectorAll('.button-row > button, .right > button'),
			(b) => b.classList.contains('cbi-button-positive') && b.textContent.trim() == _('Save')) ?? null;
	},

	enhanceModal(dlg) {
		const save = this.findModalSave(dlg);

		if (!save || dlg.querySelector('.slate-save-apply'))
			return null;

		/* Any other button (Dismiss, plain Save) cancels a pending apply. */
		dlg.querySelectorAll('.button-row > button, .right > button').forEach((b) => {
			b.addEventListener('click', () => { this.pendingApply = false; }, true);
		});

		const btn = E('button', {
			'class': 'btn cbi-button cbi-button-apply important slate-save-apply',
			'type': 'button',
			'disabled': save.disabled ? '' : null,
			'click': () => {
				if (save.disabled)
					return;

				save.click();
				this.pendingApply = true;
			}
		}, [ _('Save & Apply') ]);

		save.classList.remove('important');
		save.after(' ', btn);

		return btn;
	},

	/* The page's own Save & Apply button, when it has one. */
	pageApply() {
		return document.querySelector('.cbi-page-actions .cbi-button-apply');
	},

	applyNow() {
		const btn = this.pageApply();

		/* The page's button also saves form edits not yet staged. */
		if (btn && !btn.disabled)
			return btn.click();

		if (this.count > 0)
			return ui.changes.apply(true);
	},

	update(n) {
		this.count = Math.max(0, +n || 0);
		this.watchPageApply();
		this.render();
	},

	watchPageApply() {
		const btn = this.pageApply();

		if (btn === this.observed || typeof IntersectionObserver != 'function')
			return;

		this.io?.disconnect();
		this.observed = btn;
		this.pageApplyVisible = false;

		if (!btn)
			return;

		this.io = new IntersectionObserver((entries) => {
			this.pageApplyVisible = entries.some((e) => e.isIntersecting);
			this.render();
		});
		this.io.observe(btn);
	},

	render() {
		const show = this.count > 0 && !this.pageApplyVisible;

		if (!this.bar) {
			if (!show)
				return;

			this.bar = E('div', { 'class': 'slate-applybar', 'role': 'status', 'aria-live': 'polite' }, [
				E('span', { 'class': 'slate-applybar-text' }),
				E('div', { 'class': 'slate-applybar-actions' }, [
					E('button', { 'class': 'btn cbi-button', 'type': 'button',
						'click': () => ui.changes.displayChanges() }, [ _('Review') ]),
					E('button', { 'class': 'btn cbi-button cbi-button-reset', 'type': 'button',
						'click': () => ui.changes.revert() }, [ _('Revert') ]),
					E('button', { 'class': 'btn cbi-button cbi-button-apply important', 'type': 'button',
						'click': () => this.applyNow() }, [ _('Save & Apply') ])
				])
			]);
			document.body.appendChild(this.bar);
		}

		this.bar.querySelector('.slate-applybar-text').textContent = this.label(this.count);
		this.bar.hidden = !show;
		document.body.classList.toggle('slate-has-applybar', show);
	}
});
