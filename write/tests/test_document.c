/* Invariant and behaviour checks for the document model. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "doc/document.h"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static void check_invariants(const WpDocument *d)
{
  CHECK(d->nparas >= 1);
  for (size_t i = 0; i < d->nparas; i++) {
    const WpParagraph *p = &d->paras[i];
    CHECK(p->text != NULL);
    CHECK(p->text[p->len] == 0);
    CHECK(p->nruns >= 1);
    size_t sum = 0;
    for (size_t r = 0; r < p->nruns; r++) {
      sum += p->runs[r].len;
      if (p->nruns > 1) CHECK(p->runs[r].len > 0);
      if (r > 0) CHECK(!wp_attrs_equal(&p->runs[r - 1].attrs, &p->runs[r].attrs));
    }
    CHECK(sum == p->len);
  }
}

static char *text_of(const WpDocument *d) { return wp_document_get_all_text(d, NULL); }

int main(void)
{
  WpDocument *d = wp_document_new();
  check_invariants(d);

  /* Typing and paragraph splits */
  WpPos p = wp_document_insert_text(d, (WpPos){0, 0}, "Hello world", 11, NULL);
  CHECK(p.para == 0 && p.offset == 11);
  p = wp_document_insert_text(d, p, "\nSecond\nThird", 13, NULL);
  CHECK(d->nparas == 3 && p.para == 2 && p.offset == 5);
  char *t = text_of(d); CHECK(strcmp(t, "Hello world\nSecond\nThird") == 0); free(t);
  check_invariants(d);

  /* Bold a range spanning a paragraph break, then type after it inheriting bold */
  wp_document_set_flags(d, (WpPos){0, 6}, (WpPos){1, 3}, WP_ATTR_BOLD, true);
  check_invariants(d);
  CHECK(d->paras[0].nruns == 2 && d->paras[0].runs[1].attrs.flags == WP_ATTR_BOLD);
  CHECK(d->paras[1].nruns == 2 && d->paras[1].runs[0].attrs.flags == WP_ATTR_BOLD && d->paras[1].runs[0].len == 3);
  CHECK(wp_document_attrs_at(d, (WpPos){1, 3}).flags == WP_ATTR_BOLD);
  CHECK(wp_document_attrs_at(d, (WpPos){1, 4}).flags == 0);
  wp_document_insert_text(d, (WpPos){1, 3}, "XX", 2, NULL);
  CHECK(d->paras[1].runs[0].len == 5);
  check_invariants(d);

  /* Explicit attrs insert splits a run */
  WpTextAttrs it = { WP_ATTR_ITALIC, 0, NULL };
  wp_document_insert_text(d, (WpPos){0, 2}, "ii", 2, &it);
  CHECK(d->paras[0].nruns == 4);
  t = text_of(d); CHECK(strncmp(t, "Heiillo world", 13) == 0); free(t);
  check_invariants(d);

  /* Delete across paragraphs merges them */
  wp_document_delete_range(d, (WpPos){0, 5}, (WpPos){2, 2});
  CHECK(d->nparas == 1);
  t = text_of(d); CHECK(strcmp(t, "Heiilird") == 0); free(t);
  check_invariants(d);

  /* Enter in an empty bold paragraph keeps bold */
  wp_document_clear(d);
  wp_document_set_flags(d, (WpPos){0, 0}, (WpPos){0, 0}, WP_ATTR_BOLD, true);
  CHECK(wp_document_attrs_at(d, (WpPos){0, 0}).flags == WP_ATTR_BOLD);
  wp_document_insert_text(d, (WpPos){0, 0}, "\n", 1, NULL);
  CHECK(d->nparas == 2 && wp_document_attrs_at(d, (WpPos){1, 0}).flags == WP_ATTR_BOLD);
  check_invariants(d);

  /* Backspace at paragraph start merges */
  wp_document_delete_range(d, (WpPos){0, 0}, (WpPos){1, 0});
  CHECK(d->nparas == 1 && d->paras[0].len == 0);
  check_invariants(d);

  /* UTF-8 stepping and load/save round trip */
  { const char *src = "héllo wörld\r\nline two\n"; wp_document_load_text(d, src, strlen(src)); }
  CHECK(d->nparas == 3);
  CHECK(d->paras[0].len == 13);   /* two 2-byte chars */
  WpPos q = { 0, 1 };
  wp_document_pos_forward(d, &q); CHECK(q.offset == 3);
  wp_document_pos_backward(d, &q); CHECK(q.offset == 1);
  q = (WpPos){ 1, 0 };
  wp_document_pos_backward(d, &q); CHECK(q.para == 0 && q.offset == 13);
  WpPos s, e;
  wp_document_word_bounds(d, (WpPos){0, 8}, &s, &e);
  CHECK(s.offset == 7 && e.offset == 13);
  t = text_of(d); CHECK(strcmp(t, "héllo wörld\nline two\n") == 0); free(t);
  check_invariants(d);

  /* Selecting all and deleting leaves one empty paragraph */
  wp_document_delete_range(d, (WpPos){0, 0}, wp_document_end(d));
  CHECK(d->nparas == 1 && d->paras[0].len == 0);
  check_invariants(d);

  /* Named styles: Enter at the end of a heading yields body text; in the middle keeps the heading */
  wp_document_load_text(d, "Chapter One", 11);
  wp_document_set_style_name(d, (WpPos){0,0}, (WpPos){0,0}, "Heading 1");
  CHECK(wp_document_style_of(d, 0)->outline_level == 1);
  CHECK(wp_document_para_base_attrs(d, 0).flags & WP_ATTR_BOLD);
  CHECK(wp_document_para_base_attrs(d, 0).size_pt == 18.0f);
  CHECK(d->paras[0].style.space_before_pt == 12.0f);
  wp_document_insert_text(d, (WpPos){0,11}, "\nBody", 5, NULL);
  CHECK(strcmp(wp_document_style_of(d, 1)->name, WP_STYLE_STANDARD) == 0);
  CHECK(wp_document_para_base_attrs(d, 1).flags == 0);
  wp_document_insert_text(d, (WpPos){0,7}, "\n", 1, NULL);
  CHECK(d->nparas == 3 && wp_document_style_of(d, 1)->outline_level == 1);
  check_invariants(d);

  /* Quote and Note are built in; Enter at the end of either returns to body text */
  wp_document_load_text(d, "Quoted", 6);
  wp_document_set_style_name(d, (WpPos){0,0}, (WpPos){0,0}, "Quote");
  CHECK(wp_document_para_base_attrs(d, 0).flags & WP_ATTR_ITALIC);
  CHECK(d->paras[0].style.indent_left_pt > 0 && d->paras[0].style.indent_right_pt > 0);
  CHECK(!wp_para_style_boxed(&d->paras[0].style));
  wp_document_insert_text(d, (WpPos){0,6}, "\nAfter", 6, NULL);
  CHECK(strcmp(wp_document_style_of(d, 1)->name, WP_STYLE_STANDARD) == 0);
  wp_document_set_style_name(d, (WpPos){1,0}, (WpPos){1,0}, "Note");
  CHECK(wp_para_style_boxed(&d->paras[1].style) && wp_color_set(d->paras[1].style.background));
  CHECK(d->paras[1].style.border_left_pt > 0 && d->paras[1].style.padding_pt > 0);
  check_invariants(d);

  /* Fragments: copy a range with its formatting and paste it elsewhere */
  { const char *src = "Chapter One\nAlpha beta gamma\nDelta"; wp_document_load_text(d, src, strlen(src)); }
  wp_document_set_style_name(d, (WpPos){0,0}, (WpPos){0,0}, "Heading 1");
  wp_document_set_flags(d, (WpPos){1, 6}, (WpPos){1, 10}, WP_ATTR_BOLD, true);   /* "beta" bold */
  size_t nf = 0;
  WpParagraph *frag = wp_document_copy_range(d, (WpPos){1, 6}, (WpPos){1, 10}, &nf);
  CHECK(nf == 1 && frag[0].len == 4 && strcmp(frag[0].text, "beta") == 0);
  CHECK(frag[0].nruns == 1 && frag[0].runs[0].attrs.flags == WP_ATTR_BOLD);
  /* one paragraph joins the text at the caret, keeping its run attributes */
  WpPos after = wp_document_insert_paras(d, (WpPos){2, 5}, frag, nf);
  CHECK(after.para == 2 && after.offset == 9);
  CHECK(strcmp(d->paras[2].text, "Deltabeta") == 0);
  CHECK(wp_document_attrs_at(d, (WpPos){2, 9}).flags == WP_ATTR_BOLD);
  CHECK(wp_document_attrs_at(d, (WpPos){2, 5}).flags == 0);
  wp_paragraphs_free(frag, nf);
  check_invariants(d);

  /* a range across paragraphs is trimmed at both ends */
  frag = wp_document_copy_range(d, (WpPos){0, 8}, (WpPos){1, 5}, &nf);
  CHECK(nf == 2 && strcmp(frag[0].text, "One") == 0 && strcmp(frag[1].text, "Alpha") == 0);
  CHECK(strcmp(frag[0].style_name, "Heading 1") == 0);
  /* pasted mid-paragraph: the halves keep the paragraph's own style */
  after = wp_document_insert_paras(d, (WpPos){2, 2}, frag, nf);
  CHECK(d->nparas == 4 && after.para == 3 && after.offset == 5);
  CHECK(strcmp(d->paras[2].text, "DeOne") == 0 && strcmp(d->paras[3].text, "Alphaltabeta") == 0);
  CHECK(strcmp(wp_document_style_of(d, 2)->name, WP_STYLE_STANDARD) == 0);
  CHECK(strcmp(wp_document_style_of(d, 3)->name, WP_STYLE_STANDARD) == 0);
  /* pasted into an empty paragraph: the fragment's styles come along */
  wp_document_insert_text(d, wp_document_end(d), "\n", 1, NULL);
  after = wp_document_insert_paras(d, wp_document_end(d), frag, nf);
  CHECK(d->nparas == 6 && after.para == 5);
  CHECK(strcmp(wp_document_style_of(d, 4)->name, "Heading 1") == 0 && strcmp(d->paras[4].text, "One") == 0);
  CHECK(strcmp(wp_document_style_of(d, 5)->name, WP_STYLE_STANDARD) == 0 && strcmp(d->paras[5].text, "Alpha") == 0);
  wp_paragraphs_free(frag, nf);
  check_invariants(d);

  /* plain text has no styles and adopts the paragraph it lands in */
  frag = wp_paragraphs_from_text("one\r\ntwo\nthree", 14, &nf);
  CHECK(nf == 3 && frag[0].style_name == NULL && strcmp(frag[0].text, "one") == 0 && strcmp(frag[2].text, "three") == 0);
  after = wp_document_insert_paras(d, (WpPos){4, 3}, frag, nf);   /* end of the "One" heading */
  CHECK(d->nparas == 8 && after.para == 6 && after.offset == 5);
  CHECK(strcmp(d->paras[4].text, "Oneone") == 0 && strcmp(d->paras[5].text, "two") == 0 && strcmp(d->paras[6].text, "three") == 0);
  for (size_t i = 4; i <= 6; i++) CHECK(strcmp(wp_document_style_of(d, i)->name, "Heading 1") == 0);
  wp_paragraphs_free(frag, nf);
  check_invariants(d);

  /* Comments: anchored on runs, edges do not grow, inside does, undo snapshots */
  { const char *src = "Alpha beta gamma\nDelta"; wp_document_load_text(d, src, strlen(src)); }
  uint32_t c1 = wp_document_add_comment(d, (WpPos){0, 6}, (WpPos){0, 10}, "Ann", "2026-01-02T03:04:05", "why?");   /* "beta" */
  CHECK(c1 != 0 && wp_document_comment(d, c1) && strcmp(wp_document_comment(d, c1)->text, "why?") == 0);
  CHECK(d->paras[0].nruns == 3 && d->paras[0].runs[1].attrs.comment == c1);
  WpPos ca, cb;
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && ca.offset == 6 && cb.offset == 10);
  CHECK(wp_document_comment_at(d, (WpPos){0, 6}) == c1 && wp_document_comment_at(d, (WpPos){0, 10}) == c1);
  CHECK(wp_document_comment_at(d, (WpPos){0, 5}) == 0 && wp_document_comment_at(d, (WpPos){0, 11}) == 0);
  check_invariants(d);
  /* typing at the end of the range stays outside it, in the middle joins it */
  wp_document_insert_text(d, (WpPos){0, 10}, "X", 1, NULL);
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && cb.offset == 10);
  CHECK(wp_document_typing_attrs(d, (WpPos){0, 10}).comment == 0);
  CHECK(wp_document_typing_attrs(d, (WpPos){0, 8}).comment == c1);
  wp_document_insert_text(d, (WpPos){0, 8}, "YY", 2, NULL);
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && ca.offset == 6 && cb.offset == 12);
  wp_document_insert_text(d, (WpPos){0, 6}, "Z", 1, NULL);   /* at the start: outside */
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && ca.offset == 7 && cb.offset == 13);
  check_invariants(d);
  /* a copy of commented text pastes plain; a cut pasted back keeps the comment */
  frag = wp_document_copy_range(d, (WpPos){0, 7}, (WpPos){0, 13}, &nf);
  CHECK(nf == 1 && frag[0].runs[0].attrs.comment == c1);
  wp_document_insert_paras(d, (WpPos){1, 0}, frag, nf);
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && ca.para == 0 && cb.para == 0);   /* unchanged */
  CHECK(wp_document_comment_at(d, (WpPos){1, 1}) == 0);
  wp_document_delete_range(d, (WpPos){0, 7}, (WpPos){0, 13});
  CHECK(!wp_document_comment_range(d, c1, &ca, &cb) && wp_document_comment(d, c1));   /* orphan, still recorded */
  wp_document_insert_paras(d, (WpPos){1, 0}, frag, nf);
  CHECK(wp_document_comment_range(d, c1, &ca, &cb) && ca.para == 1 && ca.offset == 0 && cb.offset == 6);
  wp_paragraphs_free(frag, nf);
  check_invariants(d);
  /* table snapshots for undo, and removal */
  size_t nc = 0;
  WpComment *snap = wp_document_copy_comments(d, &nc);
  CHECK(nc == 1 && snap[0].id == c1);
  wp_document_set_comment_text(d, c1, "edited");
  CHECK(strcmp(wp_document_comment(d, c1)->text, "edited") == 0);
  wp_document_set_comments(d, snap, nc);
  CHECK(strcmp(wp_document_comment(d, c1)->text, "why?") == 0);
  wp_comments_free(snap, nc);
  uint32_t c2 = wp_document_add_comment(d, (WpPos){0, 0}, (WpPos){0, 5}, "Bob", "", "second");
  CHECK(c2 == c1 + 1);
  /* replies share the parent's text and go with it */
  uint32_t r1 = wp_document_add_reply(d, c1, "Cy", "", "reply");
  uint32_t r2 = wp_document_add_reply(d, r1, "Di", "", "reply to reply");
  CHECK(r1 && r2 && wp_document_comment(d, r1)->parent == c1 && wp_document_comment(d, r2)->parent == c1);
  CHECK(wp_document_comment_root(d, r2) == c1 && wp_document_comment_root(d, c1) == c1);
  CHECK(wp_document_add_reply(d, 999, "", "", "") == 0);
  WpPos ra, rb;
  CHECK(wp_document_comment_range(d, r1, &ra, &rb) && wp_document_comment_range(d, c1, &ca, &cb) && wp_pos_eq(ra, ca) && wp_pos_eq(rb, cb));
  snap = wp_document_copy_comments(d, &nc);
  CHECK(nc == 4 && snap[2].parent == c1);
  wp_comments_free(snap, nc);
  wp_document_remove_comment(d, r2);
  CHECK(!wp_document_comment(d, r2) && wp_document_comment(d, r1) && d->ncomments == 3);
  wp_document_remove_comment(d, c1);
  CHECK(!wp_document_comment(d, c1) && !wp_document_comment(d, r1) && wp_document_comment(d, c2) && d->ncomments == 1);
  CHECK(wp_document_comment_at(d, (WpPos){1, 1}) == 0);
  check_invariants(d);
  wp_document_clear(d);
  CHECK(d->ncomments == 0);

  /* lists: numbering counts along one level, resets at an ordinary
   * paragraph or a bullet, and a deeper item leaves the outer count alone */
  wp_document_clear(d);
  wp_document_insert_text(d, (WpPos){0, 0}, "a\nb\nc\nd\ne\nf\ng\nh", 15, NULL);
  wp_document_set_style_name(d, (WpPos){0, 0}, (WpPos){3, 0}, WP_STYLE_LIST_NUMBER);
  CHECK(d->paras[0].style.list_kind == WP_LIST_NUMBER && d->paras[0].style.indent_left_pt == WP_LIST_INDENT_PT);
  d->paras[1].style.list_level = 1;
  d->paras[2].style.list_level = 1;
  wp_document_set_style_name(d, (WpPos){5, 0}, (WpPos){5, 0}, WP_STYLE_LIST_BULLET);
  wp_document_set_style_name(d, (WpPos){6, 0}, (WpPos){7, 0}, WP_STYLE_LIST_NUMBER);
  int num[8];
  wp_document_list_numbers(d, num);
  CHECK(num[0] == 1 && num[1] == 1 && num[2] == 2 && num[3] == 2);   /* d continues after the nested a. b. */
  CHECK(num[4] == 0 && num[5] == 0);                                  /* body, bullet */
  CHECK(num[6] == 1 && num[7] == 2);                                  /* a new list after the bullet */
  /* Enter in an item continues it: the new paragraph is an item of the same level */
  wp_document_insert_text(d, (WpPos){1, 1}, "\n", 1, NULL);
  CHECK(d->paras[2].style.list_kind == WP_LIST_NUMBER && d->paras[2].style.list_level == 1);
  wp_document_list_numbers(d, num);
  CHECK(num[2] == 2 && num[3] == 3 && num[4] == 2);
  /* labels */
  char label[16];
  wp_list_label(WP_LIST_NUMBER, 0, 12, label); CHECK(strcmp(label, "12.") == 0);
  wp_list_label(WP_LIST_NUMBER, 1, 27, label); CHECK(strcmp(label, "aa.") == 0);
  wp_list_label(WP_LIST_NUMBER, 2, 14, label); CHECK(strcmp(label, "xiv.") == 0);
  wp_list_label(WP_LIST_NUMBER, 3, 1, label);  CHECK(strcmp(label, "1.") == 0);
  wp_list_label(WP_LIST_BULLET, 0, 0, label);  CHECK(strcmp(label, "\xe2\x80\xa2") == 0);
  wp_list_label(WP_LIST_BULLET, 4, 0, label);  CHECK(strcmp(label, "\xe2\x97\xa6") == 0);
  check_invariants(d);

  /* tab stops: kept sorted, a near position replaces, removal closes the gap and zeroes the slot */
  WpParaStyle ts = { 0 };
  wp_para_style_add_tab(&ts, 144, WP_TAB_RIGHT);
  wp_para_style_add_tab(&ts, 36, WP_TAB_LEFT);
  wp_para_style_add_tab(&ts, 72, WP_TAB_CENTER);
  CHECK(ts.ntabs == 3 && ts.tabs[0].pos_pt == 36 && ts.tabs[1].pos_pt == 72 && ts.tabs[2].pos_pt == 144);
  wp_para_style_add_tab(&ts, 72.3f, WP_TAB_DECIMAL);
  CHECK(ts.ntabs == 3 && ts.tabs[1].kind == WP_TAB_DECIMAL);
  CHECK(wp_para_style_remove_tab(&ts, 72) && ts.ntabs == 2 && ts.tabs[1].pos_pt == 144 && ts.tabs[2].pos_pt == 0);
  CHECK(!wp_para_style_remove_tab(&ts, 72));
  for (int i = 0; i < 20; i++) wp_para_style_add_tab(&ts, 200 + 10 * i, WP_TAB_LEFT);
  CHECK(ts.ntabs == WP_MAX_TABS);

  wp_document_free(d);
  printf(failures ? "%d failure(s)\n" : "all document tests passed\n", failures);
  return failures ? 1 : 0;
}
