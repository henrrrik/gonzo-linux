#pragma once

#include "markdown.h"

#include <QString>
#include <cstdint>
#include <memory>
#include <optional>

namespace WordCounter {
// Whitespace-separated tokens containing at least one letter or number;
// Markdown-only syntax tokens and punctuation do not count.
int count(const QString &text, int start = 0, int length = -1);
} // namespace WordCounter

// Implicit treap of small UTF-16 blocks ported from henrrrik/gonzo-mac.
// Stores summaries rather than a second copy of the document; edits rebuild
// only boundary blocks and inserted text in O(log n + edit).
class TextMetrics {
public:
    explicit TextMetrics(const QString &text = {});
    ~TextMetrics();

    TextMetrics(TextMetrics &&) noexcept;
    TextMetrics &operator=(TextMetrics &&) noexcept;
    TextMetrics(const TextMetrics &) = delete;
    TextMetrics &operator=(const TextMetrics &) = delete;

    int wordCount() const;
    int length() const;

    void reset(const QString &text);
    // Call right after the backing QString has been updated with `charsRemoved`
    // replaced by `charsAdded` at `position`.
    void applyEdit(const QString &newText, int position, int charsRemoved, int charsAdded);
    TextRange paragraphRange(const QString &text, TextRange covering) const;

private:
    struct Summary {
        int length = 0;
        int words = 0;
        bool startsSpace = true;
        bool endsSpace = true;
        bool hasSpace = false;
        bool firstWord = false;
        bool lastWord = false;
        std::optional<int> firstBreak;
        std::optional<int> lastBreak;

        static Summary joined(const Summary &a, const Summary &b);
    };

    struct Node {
        uint64_t priority = 0;
        Summary block;
        Summary total;
        std::unique_ptr<Node> left;
        std::unique_ptr<Node> right;

        Node(const Summary &b, uint64_t p) : priority(p), block(b), total(b) {}
        void update();
    };

    std::unique_ptr<Node> makeNode(const Summary &summary);
    static Summary summarize(const QString &text, int start, int length);
    std::unique_ptr<Node> build(const QString &text, int start, int length);
    static std::unique_ptr<Node> join(std::unique_ptr<Node> a, std::unique_ptr<Node> b);
    std::pair<std::unique_ptr<Node>, std::unique_ptr<Node>> splitBoundary(
        std::unique_ptr<Node> tree, int position);
    TextRange blockRange(int position) const;
    std::optional<int> breakBefore(int position, const QString &text) const;
    std::optional<int> breakAfter(int position, const QString &text) const;

    std::unique_ptr<Node> m_root;
    uint64_t m_seed = 42;
};
