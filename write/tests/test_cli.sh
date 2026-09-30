#!/bin/sh
# End-to-end check of gemwrite-cli: build a document from commands, save it,
# reopen it, and verify queries see what was written.
set -eu
CLI=$1
DIR=$2
rm -rf "$DIR"; mkdir -p "$DIR"
DOC=$DIR/doc.odt

"$CLI" style-h1 insert-text "Meeting notes" new-paragraph style-body \
       insert-text "Alpha beta gamma. Delta epsilon." new-paragraph \
       insert-text "Second paragraph here." save-as "$DOC" > "$DIR/out1.txt"
test -s "$DOC"

"$CLI" "$DOC" info > "$DIR/info.json"
grep -q '"paragraphs":3' "$DIR/info.json"
grep -q '"words":10' "$DIR/info.json"

# find + replace the selection, then inspect formatting at the caret
"$CLI" "$DOC" find gamma insert-text GAMMA goto 0 select 0 0:7 bold attrs save > "$DIR/attrs.json"
grep -q '"bold":true' "$DIR/attrs.json"
"$CLI" "$DOC" text | grep -q 'Alpha beta GAMMA'
"$CLI" "$DOC" paragraphs | grep -q '"style":"Heading 1"'

# script mode over stdin, with quoting
printf '%s\n' '# comment' 'goto end' 'new-paragraph' 'insert-text "Third, quoted paragraph"' 'align center' 'save' \
  | "$CLI" "$DOC" --script -
"$CLI" "$DOC" paragraphs | grep -q '"align":"center"'
"$CLI" "$DOC" render "$DIR/page0.png" 0
test -s "$DIR/page0.png"
"$CLI" "$DOC" export-pdf "$DIR/doc.pdf"
head -c 5 "$DIR/doc.pdf" | grep -q '%PDF-'

# reload: takes the file's version as one undo step, caret kept
"$CLI" insert-text "Saved text" save-as "$DIR/rl.odt" > /dev/null
"$CLI" "$DIR/rl.odt" goto 0:5 insert-text "ZZZ" reload text | grep -qx '"Saved text"' || { echo "reload should take the file's version"; exit 1; }
"$CLI" "$DIR/rl.odt" goto 0:5 insert-text "ZZZ" reload caret | grep -q '"para":0,"offset":8' || { echo "reload should keep the caret"; exit 1; }
"$CLI" "$DIR/rl.odt" goto 0:5 insert-text "ZZZ" reload undo text | grep -qx '"SavedZZZ text"' || { echo "undo should back a reload out"; exit 1; }
"$CLI" "$DIR/rl.odt" insert-text "x" reload info | grep -q '"modified":false' || { echo "reload should leave the document unmodified"; exit 1; }
"$CLI" reload 2>/dev/null && { echo "reload without a file should fail"; exit 1; }

# quote and note styles
"$CLI" style-quote insert-text "Said" new-paragraph style-note insert-text "Aside" new-paragraph insert-text "Body" paragraphs > "$DIR/styles.json"
grep -q '"index":0,"style":"Quote"' "$DIR/styles.json" && grep -q '"index":1,"style":"Note"' "$DIR/styles.json" && grep -q '"index":2,"style":"Standard"' "$DIR/styles.json" || { echo "quote/note styles"; cat "$DIR/styles.json"; exit 1; }
"$CLI" styles | grep -q '"Quote","Note"' || { echo "styles list"; exit 1; }

# lists: toggling, nesting, the ways out, numbering across a save, and the queries
"$CLI" list-number insert-text "one" new-paragraph insert-text "two" indent new-paragraph list-bullet insert-text "sub" \
       new-paragraph outdent list-number insert-text "three" new-paragraph new-paragraph insert-text "body" save-as "$DIR/list.odt" > /dev/null
"$CLI" "$DIR/list.odt" paragraphs > "$DIR/list.json"
grep -q '"index":0,"style":"List Number","align":"left","spacing":1,"indent":36,"list":"number","level":0,"text":"one"' "$DIR/list.json" || { echo "list item lost"; cat "$DIR/list.json"; exit 1; }
grep -q '"index":1,"style":"List Number","align":"left","spacing":1,"indent":72,"list":"number","level":1,"text":"two"' "$DIR/list.json" || { echo "nested item lost"; cat "$DIR/list.json"; exit 1; }
grep -q '"index":2,"style":"List Bullet","align":"left","spacing":1,"indent":72,"list":"bullet","level":1,"text":"sub"' "$DIR/list.json" || { echo "bullet under number lost"; cat "$DIR/list.json"; exit 1; }
grep -q '"index":3,"style":"List Number","align":"left","spacing":1,"indent":36,"list":"number","level":0,"text":"three"' "$DIR/list.json" || { echo "outdent lost"; cat "$DIR/list.json"; exit 1; }
grep -q '"index":4,"style":"Standard","align":"left","spacing":1,"indent":0,"list":"none","level":0,"text":"body"' "$DIR/list.json" || { echo "Enter on an empty item should leave the list"; cat "$DIR/list.json"; exit 1; }
"$CLI" "$DIR/list.odt" goto 1 attrs | grep -q '"list":"number","level":1' || { echo "attrs missing list"; exit 1; }
"$CLI" "$DIR/list.odt" goto 1 delete-backward attrs | grep -q '"list":"number","level":0' || { echo "backspace should outdent"; exit 1; }
"$CLI" "$DIR/list.odt" goto 0 delete-backward attrs | grep -q '"style":"Standard","align":"left","spacing":1,"indent":0,.*"list":"none"' || { echo "backspace at the top should leave the list"; exit 1; }
"$CLI" "$DIR/list.odt" goto 0 delete-backward undo attrs | grep -q '"list":"number","level":0' || { echo "undo of leaving a list"; exit 1; }
"$CLI" "$DIR/list.odt" goto 0 list-number attrs | grep -q '"style":"Standard"' || { echo "list-number on a numbered item should toggle it off"; exit 1; }
"$CLI" "$DIR/list.odt" goto 0 list-bullet attrs | grep -q '"list":"bullet","level":0' || { echo "list-bullet on a numbered item should switch it"; exit 1; }
"$CLI" "$DIR/list.odt" select 0 3 list-bullet paragraphs | grep -q '"index":3,"style":"List Bullet"' || { echo "list-bullet over a selection"; exit 1; }
"$CLI" "$DIR/list.odt" select 0 4 list-bullet paragraphs | grep -q '"index":4,"style":"List Bullet"' || { echo "list-bullet should include a body paragraph in the selection"; exit 1; }
"$CLI" "$DIR/list.odt" select 0 4 list-bullet list-bullet paragraphs | grep -q '"index":0,"style":"Standard"' || { echo "list-bullet twice should clear the selection's list"; exit 1; }
"$CLI" style-h1 insert-text Head list-bullet attrs | grep -q '"style":"List Bullet"' || { echo "list-bullet on a heading"; exit 1; }
# a list is formatting: alignment and spacing survive going in and coming out
"$CLI" insert-text x align center spacing-double list-bullet attrs | grep -q '"align":"center","spacing":2,"indent":36,.*"list":"bullet"' || { echo "list-bullet should keep alignment and spacing"; exit 1; }
"$CLI" insert-text x align center spacing-double list-bullet list-bullet attrs | grep -q '"style":"Standard","align":"center","spacing":2' || { echo "leaving a list should keep alignment and spacing"; exit 1; }
"$CLI" insert-text x spacing-1.5 list-number indent goto 0:0 delete-backward delete-backward attrs | grep -q '"style":"Standard","align":"left","spacing":1.5' || { echo "stepping out should keep spacing"; exit 1; }
"$CLI" list-bullet indent indent indent indent indent indent indent indent indent indent indent attrs | grep -q '"level":9' || { echo "indent should stop at level 9"; exit 1; }
# indent on an ordinary paragraph moves its left edge by half an inch, within the margins
"$CLI" insert-text x indent attrs | grep -q '"indent":36' || { echo "indent should move the paragraph"; exit 1; }
"$CLI" insert-text x indent indent outdent attrs | grep -q '"indent":36' || { echo "outdent should move it back"; exit 1; }
"$CLI" insert-text x outdent attrs | grep -q '"indent":0' || { echo "outdent at the margin should stay put and not fail"; exit 1; }
"$CLI" insert-text x indent indent indent indent indent indent indent indent indent indent indent indent attrs | grep -q '"indent":396' || { echo "indent should stop an inch short of the right margin"; cat; exit 1; }
"$CLI" insert-text x indent undo attrs | grep -q '"indent":0' || { echo "indent undo"; exit 1; }
"$CLI" insert-text a new-paragraph list-bullet insert-text b select 0 1 indent paragraphs > "$DIR/indent.json"
grep -q '"index":0,"style":"Standard","align":"left","spacing":1,"indent":36' "$DIR/indent.json" && grep -q '"index":1,"style":"List Bullet","align":"left","spacing":1,"indent":72,"list":"bullet","level":1' "$DIR/indent.json" || { echo "indent over a mixed selection"; cat "$DIR/indent.json"; exit 1; }
"$CLI" insert-text x indent save-as "$DIR/indent.odt" > /dev/null && "$CLI" "$DIR/indent.odt" attrs | grep -q '"indent":36' || { echo "indent lost in the file"; exit 1; }
"$CLI" "$DIR/list.odt" goto 3 export-markdown "$DIR/list.md" > /dev/null
printf '%s\n' '1. one' '    1. two' '    - sub' '2. three' '' 'body' | diff - "$DIR/list.md" || { echo "list markdown differs"; exit 1; }
"$CLI" "$DIR/list.odt" render "$DIR/list.png" 0 && test -s "$DIR/list.png"
"$CLI" styles | grep -q '"List Bullet","List Number"' || { echo "list styles missing"; exit 1; }

# indents by value and tab stops: clamped to the page, saved, reported by attrs
"$CLI" insert-text x indent-left 72 indent-right 36 indent-first -36 attrs | grep -q '"indent":72,"indent_right":36,"indent_first":-36' || { echo "indent-left/right/first"; exit 1; }
"$CLI" insert-text x indent-left 9999 attrs | grep -q '"indent":396,' || { echo "indent-left should stop an inch short of the right margin"; exit 1; }
"$CLI" insert-text x indent-first -50 attrs | grep -q '"indent_first":0' || { echo "a first line cannot hang past the margin"; exit 1; }
"$CLI" insert-text x list-bullet indent-first 20 attrs | grep -q '"indent_first":0' || { echo "a list item has no first-line indent"; exit 1; }
"$CLI" insert-text x tab-add 144 right tab-add 36 left tab-add 200 decimal attrs | grep -q '"tabs":\[{"pos":36,"type":"left"},{"pos":144,"type":"right"},{"pos":200,"type":"decimal"}\]' || { echo "tab-add"; exit 1; }
"$CLI" insert-text x tab-add 144 right tab-remove 144 attrs | grep -q '"tabs":\[\]' || { echo "tab-remove"; exit 1; }
"$CLI" insert-text x tab-add 144 right tab-add 72 left tab-clear attrs | grep -q '"tabs":\[\]' || { echo "tab-clear"; exit 1; }
"$CLI" insert-text x tab-remove 10 2>/dev/null && { echo "removing a missing tab stop should fail"; exit 1; }
"$CLI" insert-text x tab-add 10 wobbly 2>/dev/null && { echo "an unknown tab type should fail"; exit 1; }
"$CLI" insert-text x tab-add 100 center indent-first 18 undo undo attrs | grep -q '"indent_first":0,"tabs":\[\]' || { echo "indent and tab undo"; exit 1; }
"$CLI" insert-text "$(printf 'a\tb')" indent-first 18 tab-add 100 center save-as "$DIR/tabs.odt" > /dev/null
"$CLI" "$DIR/tabs.odt" attrs | grep -q '"indent_first":18,"tabs":\[{"pos":100,"type":"center"}\]' || { echo "indent and tabs lost in the file"; exit 1; }
"$CLI" "$DIR/tabs.odt" render "$DIR/tabs.png" 0 && test -s "$DIR/tabs.png"

# line spacing: applied per paragraph, saved and reloaded, shown by queries
"$CLI" insert-text "one" spacing-double new-paragraph insert-text "two" line-spacing 1.15 save-as "$DIR/spacing.odt" > /dev/null
"$CLI" "$DIR/spacing.odt" paragraphs > "$DIR/spacing.json"
grep -q '"index":0,"style":"Standard","align":"left","spacing":2' "$DIR/spacing.json" || { echo "double spacing lost"; cat "$DIR/spacing.json"; exit 1; }
grep -q '"index":1,"style":"Standard","align":"left","spacing":1.15' "$DIR/spacing.json" || { echo "1.15 spacing lost"; cat "$DIR/spacing.json"; exit 1; }
"$CLI" "$DIR/spacing.odt" goto 0 attrs | grep -q '"spacing":2' || { echo "attrs missing spacing"; exit 1; }
"$CLI" "$DIR/spacing.odt" goto 0 spacing-1.5 undo attrs | grep -q '"spacing":2' || { echo "spacing undo failed"; exit 1; }
"$CLI" line-spacing 0 2>/dev/null && { echo "line-spacing 0 should fail"; exit 1; }

# comments: add on a selection, list, edit, delete, undo, and survive a save
"$CLI" insert-text "Alpha beta gamma" select 0:6 0:10 comment-add "Check this" goto end insert-text "!" save-as "$DIR/cm.odt" > "$DIR/cm1.json"
grep -q '"id":1' "$DIR/cm1.json" || { echo "comment-add should report the id"; exit 1; }
"$CLI" "$DIR/cm.odt" comments > "$DIR/cm2.json"
grep -q '"id":1,"parent":0,"author":"[^"]*","date":"[0-9T:-]*","text":"Check this","start":{"para":0,"offset":6},"end":{"para":0,"offset":10}' "$DIR/cm2.json" || { echo "comments query after reload"; cat "$DIR/cm2.json"; exit 1; }
"$CLI" "$DIR/cm.odt" goto 0:8 attrs | grep -q '"comment":1' || { echo "attrs should show the comment at the caret"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-edit 1 "Changed" comments | grep -q '"text":"Changed"' || { echo "comment-edit failed"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-edit 1 "Changed" undo comments | grep -q '"text":"Check this"' || { echo "undo of comment-edit failed"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-delete 1 comments | grep -qx '\[\]' || { echo "comment-delete failed"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-delete 1 undo comments | grep -q '"id":1' || { echo "undo of comment-delete failed"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-select 1 selection | grep -qx '"beta"' || { echo "comment-select failed"; exit 1; }
"$CLI" "$DIR/cm.odt" select 0:6 0:10 delete-selection comments | grep -qx '\[\]' || { echo "deleted text should hide its comment"; exit 1; }
"$CLI" "$DIR/cm.odt" select 0:6 0:10 delete-selection undo comments | grep -q '"id":1' || { echo "undo should bring the comment back"; exit 1; }
"$CLI" comment-add "x" 2>/dev/null && { echo "comment-add without a selection should fail"; exit 1; }
"$CLI" comment-delete 7 2>/dev/null && { echo "deleting a missing comment should fail"; exit 1; }

# --author signs that run's comments
"$CLI" --author "Review Bot" "$DIR/cm.odt" select 0:0 0:5 comment-add "From the bot" comments | grep -q '"author":"Review Bot","date":"[0-9T:-]*","text":"From the bot"' || { echo "--author ignored"; exit 1; }
"$CLI" --author 2>/dev/null && { echo "--author without a name should fail"; exit 1; }

# replies: nest under the parent, go with it, and come back with undo
"$CLI" "$DIR/cm.odt" comment-reply 1 "Agreed" comment-reply 1 "Me too" save > /dev/null
"$CLI" "$DIR/cm.odt" comments > "$DIR/cm3.json"
grep -q '"id":2,"parent":1,"author":"[^"]*","date":"[0-9T:-]*","text":"Agreed"' "$DIR/cm3.json" || { echo "reply missing after reload"; cat "$DIR/cm3.json"; exit 1; }
grep -q '"id":3,"parent":1,' "$DIR/cm3.json" || { echo "second reply missing"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-reply 3 "Nested" comments | grep -q '"id":4,"parent":1,' || { echo "reply to a reply should join the thread"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-delete 2 comments | grep -q '"id":3' || { echo "deleting a reply should keep the rest"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-delete 1 comments | grep -qx '\[\]' || { echo "deleting the parent should take its replies"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-delete 1 undo comments | grep -q '"id":3,"parent":1' || { echo "undo should restore the thread"; exit 1; }
"$CLI" "$DIR/cm.odt" comment-reply 9 "x" 2>/dev/null && { echo "replying to a missing comment should fail"; exit 1; }

# undo/redo: typing coalesces into one step, formatting is its own step
"$CLI" insert-text "abc" insert-text "def" select 0 0:3 bold undo text > "$DIR/undo1.txt"
grep -qx '"abcdef"' "$DIR/undo1.txt" || { echo "undo of formatting failed"; cat "$DIR/undo1.txt"; exit 1; }
"$CLI" insert-text "abc" insert-text "def" undo text | grep -qx '""' || { echo "undo of typing should remove all of it"; exit 1; }
"$CLI" insert-text "abc" new-paragraph insert-text "def" undo text | grep -qx '"abc\\n"' || { echo "paragraph break should be its own step"; exit 1; }
"$CLI" insert-text "abc" undo redo text | grep -qx '"abc"' || { echo "redo failed"; exit 1; }
"$CLI" insert-text "abc" undo insert-text "x" redo 2>/dev/null && { echo "redo after a new edit should fail"; exit 1; }
# undo back to the saved state clears the modified flag
"$CLI" "$DOC" insert-text "zzz" undo info | grep -q '"modified":false' || { echo "modified flag after undo"; exit 1; }

# cut/copy/paste: formatting travels with the text, paste is one undo step
"$CLI" insert-text "Hello world" select 0:0 0:5 copy goto end paste text | grep -qx '"Hello worldHello"' || { echo "copy/paste failed"; exit 1; }
"$CLI" insert-text "Hello world" select 0:0 0:6 cut text | grep -qx '"world"' || { echo "cut failed"; exit 1; }
"$CLI" insert-text "abc" select start end bold copy goto end paste goto 0:5 attrs | grep -q '"bold":true' || { echo "paste lost formatting"; exit 1; }
"$CLI" style-h1 insert-text Head new-paragraph insert-text Body select start end copy goto end new-paragraph paste paragraphs > "$DIR/paste.json"
grep -q '"index":2,"style":"Heading 1"' "$DIR/paste.json" || { echo "paste lost paragraph styles"; cat "$DIR/paste.json"; exit 1; }
grep -q '"paragraphs":4' <("$CLI" style-h1 insert-text Head new-paragraph insert-text Body select start end copy goto end new-paragraph paste info) || { echo "paste paragraph count"; exit 1; }
"$CLI" insert-text "abc" select start end copy goto end paste paste undo text | grep -qx '"abcabc"' || { echo "paste should be one undo step"; exit 1; }
"$CLI" copy 2>/dev/null && { echo "copy without a selection should fail"; exit 1; }
"$CLI" paste 2>/dev/null && { echo "paste with an empty clipboard should fail"; exit 1; }

# export-markdown: styles and formatting map to Markdown, the document keeps its own file
"$CLI" style-title insert-text "Report" new-paragraph style-h2 insert-text "Part one" new-paragraph \
       insert-text "Some " bold insert-text "bold" bold insert-text " text." new-paragraph style-quote insert-text "Quoted" new-paragraph \
       style-note insert-text "Aside" save-as "$DIR/md.odt" export-markdown "$DIR/md.md" info > "$DIR/md.json"
printf '%s\n' '# Report' '' '## Part one' '' 'Some **bold** text.' '' '> Quoted' '' '> [!NOTE]' '> Aside' | diff - "$DIR/md.md" || { echo "markdown export differs"; exit 1; }
grep -q '"path":"[^"]*md.odt"' "$DIR/md.json" || { echo "export should not change the document's path"; cat "$DIR/md.json"; exit 1; }
"$CLI" "$DIR/md.odt" insert-text "x" export-markdown "$DIR/md2.md" info | grep -q '"modified":true' || { echo "export should not clear the modified flag"; exit 1; }
"$CLI" export-markdown "$DIR/nope/x.md" 2>/dev/null && { echo "export to a missing directory should fail"; exit 1; }
"$CLI" --list | grep -q 'export-markdown FILE' || { echo "export-markdown missing from --list"; exit 1; }

# spelling: needs an English dictionary; spell-add is not run (it would write to the user's word list)
if "$CLI" spell-languages | grep -q '"en'; then
  "$CLI" insert-text "Helo world, it's fine." misspellings > "$DIR/spell.json"
  grep -q '^\[{"word":"Helo","start":{"para":0,"offset":0},"end":{"para":0,"offset":4},"suggestions":\[.*"Hello".*\]}\]$' "$DIR/spell.json" || { echo "misspellings query"; cat "$DIR/spell.json"; exit 1; }
  "$CLI" insert-text "Helo world" goto 0:2 spell-ignore misspellings | grep -qx '\[\]' || { echo "spell-ignore should hide the word"; exit 1; }
  "$CLI" insert-text "Helo world" goto 0:7 spell-ignore 2>/dev/null && { echo "spell-ignore off a misspelling should fail"; exit 1; }
  "$CLI" insert-text "Helo" spell-language en misspellings | grep -q '"word":"Helo"' || { echo "spell-language en should find a dictionary"; exit 1; }
  "$CLI" spell-language xx_XX 2>/dev/null && { echo "spell-language with no dictionary should fail"; exit 1; }
else
  echo "no English dictionary installed: spelling commands not tested"
fi

# find and replace: case-insensitive with wrap-around, match-case, replace
# keeps formatting, replace-all is one undo step
"$CLI" insert-text "Cat cat CAT." new-paragraph insert-text "a cat" find cat find-next find-next find-next find-next selection | tail -1 | grep -qx '"Cat"' || { echo "find-next should wrap"; exit 1; }
"$CLI" insert-text "Cat cat CAT." find cat find-previous selection | tail -1 | grep -qx '"CAT"' || { echo "find-previous should wrap"; exit 1; }
"$CLI" insert-text "Cat cat CAT." match-case true find cat find-next selection | tail -1 | grep -qx '"cat"' || { echo "match-case"; exit 1; }
"$CLI" insert-text "one two three" select 0:4 0:7 bold find two replace TWO goto 0:5 attrs text > "$DIR/replace.txt"
grep -q '"bold":true' "$DIR/replace.txt" && grep -qx '"one TWO three"' "$DIR/replace.txt" || { echo "replace should keep formatting"; cat "$DIR/replace.txt"; exit 1; }
"$CLI" insert-text "Cat cat" find cat goto 0 replace dog 2>/dev/null && { echo "replace off a match should fail"; exit 1; }
"$CLI" insert-text "Cat cat CAT." new-paragraph insert-text "a cat" replace-all cat kitten text undo text > "$DIR/all.txt"
grep -q '"replaced":4' "$DIR/all.txt" && grep -qxF '"kitten kitten kitten.\na kitten"' "$DIR/all.txt" && grep -qxF '"Cat cat CAT.\na cat"' "$DIR/all.txt" || { echo "replace-all"; cat "$DIR/all.txt"; exit 1; }
"$CLI" insert-text "Ärger" find ärger selection | tail -1 | grep -qx '"Ärger"' || { echo "find should fold case beyond ASCII"; exit 1; }
"$CLI" insert-text "abc" replace-all zzz y 2>/dev/null && { echo "replace-all with no match should fail"; exit 1; }

# errors stop the run and report
if "$CLI" "$DOC" find "no such text" 2> "$DIR/err.txt"; then echo "expected failure"; exit 1; fi
grep -q 'not found' "$DIR/err.txt"
echo "cli ok"
