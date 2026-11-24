#include "textmetrics.h"

#include <QChar>
#include <algorithm>

namespace {

constexpr int kBlockSize = 1024;

inline bool isSpaceUnit(ushort c) {
    if (c < 128)
        return c == 32 || (c >= 9 && c <= 13);
    return QChar(c).isSpace();
}

inline bool isLetterUnit(ushort c) {
    if (c < 128)
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
    return QChar(c).isLetterOrNumber();
}

inline bool isBreakUnit(ushort c) {
    return c == 10 || c == 13 || c == 0x2028 || c == 0x2029;
}

} // namespace

namespace WordCounter {

int count(const QString &text, int start, int length) {
    const int size = text.size();
    const int begin = std::clamp(start, 0, size);
    const int end = length < 0 ? size : std::clamp(begin + length, begin, size);
    const QChar *data = text.constData();

    int words = 0;
    bool inWord = false;
    for (int i = begin; i < end; ++i) {
        const ushort c = data[i].unicode();
        if (isSpaceUnit(c)) {
            inWord = false;
        } else if (!inWord && isLetterUnit(c)) {
            ++words;
            inWord = true;
        } else if (!inWord && c >= 0xD800 && c <= 0xDBFF && i + 1 < end) {
            const ushort low = data[i + 1].unicode();
            if (low >= 0xDC00 && low <= 0xDFFF) {
                const uint scalar = QChar::surrogateToUcs4(c, low);
                if (QChar::isLetterOrNumber(scalar)) {
                    ++words;
                    inWord = true;
                }
            }
        }
    }
    return words;
}

} // namespace WordCounter

TextMetrics::Summary TextMetrics::Summary::joined(const Summary &a, const Summary &b) {
    if (a.length == 0)
        return b;
    if (b.length == 0)
        return a;
    const bool merge = !a.endsSpace && !b.startsSpace;
    Summary out;
    out.length = a.length + b.length;
    out.words = a.words + b.words - (merge && a.lastWord && b.firstWord ? 1 : 0);
    out.startsSpace = a.startsSpace;
    out.endsSpace = b.endsSpace;
    out.hasSpace = a.hasSpace || b.hasSpace;
    out.firstWord = a.firstWord || (!a.hasSpace && !b.startsSpace && b.firstWord);
    out.lastWord = b.lastWord || (!b.hasSpace && !a.endsSpace && a.lastWord);
    out.firstBreak = a.firstBreak.has_value()
        ? a.firstBreak
        : (b.firstBreak.has_value() ? std::optional<int>(a.length + *b.firstBreak) : std::nullopt);
    out.lastBreak = b.lastBreak.has_value()
        ? std::optional<int>(a.length + *b.lastBreak)
        : a.lastBreak;
    return out;
}

void TextMetrics::Node::update() {
    const Summary leftTotal = left ? left->total : Summary{};
    const Summary rightTotal = right ? right->total : Summary{};
    total = Summary::joined(Summary::joined(leftTotal, block), rightTotal);
}

TextMetrics::TextMetrics(const QString &text) {
    reset(text);
}

TextMetrics::~TextMetrics() = default;
TextMetrics::TextMetrics(TextMetrics &&) noexcept = default;
TextMetrics &TextMetrics::operator=(TextMetrics &&) noexcept = default;

int TextMetrics::wordCount() const {
    return m_root ? m_root->total.words : 0;
}

int TextMetrics::length() const {
    return m_root ? m_root->total.length : 0;
}

void TextMetrics::reset(const QString &text) {
    m_root = build(text, 0, static_cast<int>(text.size()));
}

std::unique_ptr<TextMetrics::Node> TextMetrics::makeNode(const Summary &summary) {
    m_seed = m_seed * 6364136223846793005ULL + 1ULL;
    return std::make_unique<Node>(summary, m_seed);
}

TextMetrics::Summary TextMetrics::summarize(const QString &text, int start, int length) {
    Summary result;
    result.length = length;
    if (length <= 0)
        return result;

    const int textLen = static_cast<int>(text.size());
    const QChar *data = text.constData();
    bool inWord = false;
    bool firstToken = true;

    for (int i = start; i < start + length; ++i) {
        const ushort c = data[i].unicode();
        const bool space = isSpaceUnit(c);
        if (i == start)
            result.startsSpace = space;
        result.endsSpace = space;

        if (isBreakUnit(c)) {
            if (!result.firstBreak.has_value())
                result.firstBreak = i - start;
            result.lastBreak = i - start;
        }

        if (space) {
            result.hasSpace = true;
            inWord = false;
            firstToken = false;
            result.lastWord = false;
        } else {
            bool letter = isLetterUnit(c);
            // Classify both halves so summaries compose across a block boundary
            // inside a supplementary Unicode letter.
            if (c >= 0xD800 && c <= 0xDFFF) {
                const int highIndex = c <= 0xDBFF ? i : i - 1;
                if (highIndex >= 0 && highIndex + 1 < textLen) {
                    const ushort high = data[highIndex].unicode();
                    const ushort low = data[highIndex + 1].unicode();
                    if (high >= 0xD800 && high <= 0xDBFF && low >= 0xDC00 && low <= 0xDFFF) {
                        letter = QChar::isLetterOrNumber(QChar::surrogateToUcs4(high, low));
                    }
                }
            }
            if (letter) {
                if (!inWord) {
                    ++result.words;
                    inWord = true;
                }
                if (firstToken)
                    result.firstWord = true;
                result.lastWord = true;
            }
        }
    }
    return result;
}

std::unique_ptr<TextMetrics::Node> TextMetrics::build(const QString &text, int start, int length) {
    std::unique_ptr<Node> tree;
    const int end = start + length;
    int cursor = start;
    while (cursor < end) {
        const int chunk = std::min(kBlockSize, end - cursor);
        tree = join(std::move(tree), makeNode(summarize(text, cursor, chunk)));
        cursor += chunk;
    }
    return tree;
}

std::unique_ptr<TextMetrics::Node> TextMetrics::join(std::unique_ptr<Node> a, std::unique_ptr<Node> b) {
    if (!a)
        return b;
    if (!b)
        return a;
    if (a->priority > b->priority) {
        a->right = join(std::move(a->right), std::move(b));
        a->update();
        return a;
    }
    b->left = join(std::move(a), std::move(b->left));
    b->update();
    return b;
}

std::pair<std::unique_ptr<TextMetrics::Node>, std::unique_ptr<TextMetrics::Node>>
TextMetrics::splitBoundary(std::unique_ptr<Node> tree, int position) {
    if (!tree)
        return {nullptr, nullptr};
    const int leftLen = tree->left ? tree->left->total.length : 0;
    if (position <= leftLen) {
        auto [a, b] = splitBoundary(std::move(tree->left), position);
        tree->left = std::move(b);
        tree->update();
        return {std::move(a), std::move(tree)};
    }
    const int selfEnd = leftLen + tree->block.length;
    if (position >= selfEnd) {
        auto [a, b] = splitBoundary(std::move(tree->right), position - selfEnd);
        tree->right = std::move(a);
        tree->update();
        return {std::move(tree), std::move(b)};
    }
    // Callers only split on block boundaries obtained from blockRange().
    // If ever called mid-block, keep the whole block on the right safely.
    auto left = std::move(tree->left);
    tree->left.reset();
    tree->update();
    return {std::move(left), std::move(tree)};
}

TextRange TextMetrics::blockRange(int position) const {
    const Node *current = m_root.get();
    int offset = 0;
    while (current) {
        const int start = offset + (current->left ? current->left->total.length : 0);
        const int end = start + current->block.length;
        if (position < start) {
            current = current->left.get();
        } else if (position >= end) {
            offset = end;
            current = current->right.get();
        } else {
            return TextRange{start, current->block.length};
        }
    }
    return TextRange{offset, 0};
}

void TextMetrics::applyEdit(const QString &newText, int position, int charsRemoved, int charsAdded) {
    const int oldLength = length();
    if (oldLength == 0 || position < 0 || charsRemoved < 0 || charsAdded < 0
        || position + charsRemoved > oldLength
        || oldLength - charsRemoved + charsAdded != static_cast<int>(newText.size())) {
        reset(newText);
        return;
    }

    // Include neighboring blocks so surrogate pairs and merged words at edit
    // edges are re-summarized cleanly, and splits always land on block edges.
    const int start = blockRange(std::max(0, position - 1)).start;
    const int endPos = std::min(std::max(0, oldLength - 1), position + charsRemoved);
    const int end = blockRange(endPos).end();

    auto [prefix, rest] = splitBoundary(std::move(m_root), start);
    auto [replaced, suffix] = splitBoundary(std::move(rest), end - start);
    (void)replaced;

    const int middleLen = (end - start) + charsAdded - charsRemoved;
    auto middle = build(newText, start, middleLen);
    m_root = join(join(std::move(prefix), std::move(middle)), std::move(suffix));
}

std::optional<int> TextMetrics::breakBefore(int position, const QString &text) const {
    const Node *current = m_root.get();
    const QChar *data = text.constData();
    int offset = 0;
    std::optional<int> candidate;

    while (current) {
        const int start = offset + (current->left ? current->left->total.length : 0);
        if (position <= start) {
            current = current->left.get();
            continue;
        }
        if (current->left && current->left->total.lastBreak.has_value())
            candidate = offset + *current->left->total.lastBreak;
        const int end = start + current->block.length;
        if (position <= end) {
            for (int i = position - 1; i >= start; --i) {
                if (isBreakUnit(data[i].unicode()))
                    return i;
            }
            return candidate;
        }
        if (current->block.lastBreak.has_value())
            candidate = start + *current->block.lastBreak;
        offset = end;
        current = current->right.get();
    }
    return candidate;
}

std::optional<int> TextMetrics::breakAfter(int position, const QString &text) const {
    const Node *current = m_root.get();
    const QChar *data = text.constData();
    int offset = 0;
    std::optional<int> candidate;

    while (current) {
        const int start = offset + (current->left ? current->left->total.length : 0);
        const int end = start + current->block.length;
        if (position >= end) {
            offset = end;
            current = current->right.get();
            continue;
        }
        if (current->right && current->right->total.firstBreak.has_value())
            candidate = end + *current->right->total.firstBreak;
        if (position >= start) {
            for (int i = position; i < end; ++i) {
                if (isBreakUnit(data[i].unicode()))
                    return i;
            }
            return candidate;
        }
        if (current->block.firstBreak.has_value())
            candidate = start + *current->block.firstBreak;
        current = current->left.get();
    }
    return candidate;
}

TextRange TextMetrics::paragraphRange(const QString &text, TextRange covering) const {
    const int textLen = static_cast<int>(text.size());
    if (textLen != length())
        return Paragraphs::range(text, covering);

    int pos = std::clamp(covering.start, 0, textLen);
    if (pos > 0 && pos < textLen && text.at(pos).unicode() == 10 && text.at(pos - 1).unicode() == 13)
        --pos;
    const int start = breakBefore(pos, text).value_or(-1) + 1;

    int end = std::clamp(covering.end(), start, textLen);
    if (end > 0 && covering.length > 0 && isBreakUnit(text.at(end - 1).unicode())) {
        if (end < textLen && text.at(end - 1).unicode() == 13 && text.at(end).unicode() == 10)
            ++end;
    } else if (const auto next = breakAfter(end, text); next.has_value()) {
        end = *next + 1;
        if (end < textLen && text.at(*next).unicode() == 13 && text.at(end).unicode() == 10)
            ++end;
    } else {
        end = textLen;
    }
    return TextRange{start, end - start};
}
