#include "SourceTree.hpp"
#include "SourceTreeDelegate.hpp"
#include "moc_SourceTreeDelegate.cpp"

SourceTreeDelegate::SourceTreeDelegate(QObject *parent) : QStyledItemDelegate(parent) {}

QSize SourceTreeDelegate::sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const
{
	SourceTree *tree = qobject_cast<SourceTree *>(parent());
	QWidget *item = tree->indexWidget(index);

	if (!item) {
		return QStyledItemDelegate::sizeHint(option, index);
	}

	QSize hint = item->sizeHint();
#ifdef __ANDROID__
	/* Keep phone rows compact without clipping the embedded source controls. */
	hint.setHeight(qMax(hint.height(), 32));
#endif
	return hint;
}
