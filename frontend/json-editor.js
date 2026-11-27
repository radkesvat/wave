import {
    createJSONEditor, EditorView, ensureSyntaxTree, foldNodeProp, foldEffect, unfoldEffect, foldState
} from "./json-editor/vanilla-jsoneditor.js";
import "./json-editor/lossless-json.js";

const { parse, stringify } = globalThis.LosslessJSON;

await new Promise((resolve, reject) => {
    const stylesheet = document.createElement("link");
    stylesheet.rel = "stylesheet";
    stylesheet.href = new URL("./json-editor/jse-theme-dark.css", import.meta.url);
    stylesheet.addEventListener("load", resolve, { once: true });
    stylesheet.addEventListener("error", reject, { once: true });
    document.head.append(stylesheet);
});

function foldTextToLevel(target, level) {
    const element = target.querySelector(".cm-editor");
    const view = element && EditorView.findFromDOM(element);
    if (!view) return;

    const { state } = view;
    const tree = ensureSyntaxTree(state, state.doc.length, Infinity);
    if (!tree) return;

    const effects = [];
    state.field(foldState, false)?.between(0, state.doc.length, (from, to) => {
        effects.push(unfoldEffect.of({ from, to }));
    });
    const isContainer = node => node.name === "Object" || node.name === "Array";
    let depth = 0;
    tree.iterate({
        enter(node) {
            if (!isContainer(node)) return;
            depth += 1;
            if (depth === level) {
                const range = node.type.prop(foldNodeProp)?.(node.node, state);
                if (range) effects.push(foldEffect.of(range));
                depth -= 1;
                return false;
            }
        },
        leave(node) {
            if (isContainer(node)) depth -= 1;
        }
    });
    if (effects.length) view.dispatch({ effects });
}

export function createConfigJsonEditor(target, text, readOnly, ariaLabel) {
    let mode = "text";
    let editedText = false;
    const editor = createJSONEditor({
        target,
        props: {
            content: { text },
            mode,
            parser: { parse, stringify },
            readOnly,
            ariaLabel,
            onRenderMenu(items, { mode: menuMode, modal }) {
                if (modal || (menuMode !== "text" && menuMode !== "tree")) return;
                const collapseIndex = items.findIndex(item => item.className === "jse-collapse-all");
                if (collapseIndex < 0) return;

                const levels = [2, 3, 4].map(level => ({
                    type: "button",
                    text: `Level ${level}`,
                    title: `Fold to level ${level}`,
                    className: "wave-json-fold-level",
                    disabled: items[collapseIndex].disabled,
                    onClick() {
                        if (menuMode === "text") {
                            foldTextToLevel(target, level);
                        } else {
                            editor.collapse([], true);
                            editor.expand([], path => path.length < level - 1);
                        }
                    }
                }));
                return [...items.slice(0, collapseIndex + 1), ...levels, ...items.slice(collapseIndex + 1)];
            },
            onChange(_content, _previous, { contentErrors }) {
                if (mode === "text") editedText = !contentErrors?.parseError;
            },
            onChangeMode(nextMode) {
                const refreshTree = mode === "text" && nextMode === "tree" && editedText;
                mode = nextMode;
                editedText = false;
                if (refreshTree) {
                    // Rebuild Tree value controls after Text changes the document structure.
                    editor.collapse([]);
                    editor.expand([]);
                }
            }
        }
    });

    return {
        getText() {
            // Validation flushes any debounced text changes before get reads the contents.
            editor.validate();
            const content = editor.get();
            return typeof content.text === "string" ? content.text : stringify(content.json, null, 2);
        },
        setReadOnly(value) { editor.updateProps({ readOnly: value }); },
        focus() { editor.focus(); },
        refresh() { return editor.refresh(); },
        destroy() { return editor.destroy(); }
    };
}
