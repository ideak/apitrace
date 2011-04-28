#include "mainwindow.h"

#include "apitrace.h"
#include "apitracecall.h"
#include "apicalldelegate.h"
#include "apitracemodel.h"
#include "apitracefilter.h"
#include "argumentseditor.h"
#include "imageviewer.h"
#include "jumpwidget.h"
#include "retracer.h"
#include "searchwidget.h"
#include "settingsdialog.h"
#include "shaderssourcewidget.h"
#include "tracedialog.h"
#include "traceprocess.h"
#include "ui_retracerdialog.h"
#include "vertexdatainterpreter.h"

#include <QAction>
#include <QApplication>
#include <QDebug>
#include <QDesktopServices>
#include <QDesktopWidget>
#include <QDir>
#include <QFileDialog>
#include <QLineEdit>
#include <QMessageBox>
#include <QProgressBar>
#include <QToolBar>
#include <QUrl>
#include <QVBoxLayout>
#include <QWebPage>
#include <QWebView>


MainWindow::MainWindow()
    : QMainWindow(),
      m_selectedEvent(0),
      m_stateEvent(0),
      m_nonDefaultsLookupEvent(0)
{
    m_ui.setupUi(this);
    initObjects();
    initConnections();
}

void MainWindow::createTrace()
{
    TraceDialog dialog;

    if (!m_traceProcess->canTrace()) {
        QMessageBox::warning(
            this,
            tr("Unsupported"),
            tr("Current configuration doesn't support tracing."));
        return;
    }

    if (dialog.exec() == QDialog::Accepted) {
        qDebug()<< "App : " <<dialog.applicationPath();
        qDebug()<< "  Arguments: "<<dialog.arguments();
        m_traceProcess->setExecutablePath(dialog.applicationPath());
        m_traceProcess->setArguments(dialog.arguments());
        m_traceProcess->start();
    }
}

void MainWindow::openTrace()
{
    QString fileName =
        QFileDialog::getOpenFileName(
            this,
            tr("Open Trace"),
            QDir::homePath(),
            tr("Trace Files (*.trace)"));

    if (!fileName.isEmpty() && QFile::exists(fileName)) {
        newTraceFile(fileName);
    }
}

void MainWindow::loadTrace(const QString &fileName)
{
    if (!QFile::exists(fileName)) {
        QMessageBox::warning(this, tr("File Missing"),
                             tr("File '%1' doesn't exist.").arg(fileName));
        return;
    }

    newTraceFile(fileName);
}

void MainWindow::callItemSelected(const QModelIndex &index)
{
    ApiTraceEvent *event =
        index.data(ApiTraceModel::EventRole).value<ApiTraceEvent*>();

    if (event && event->type() == ApiTraceEvent::Call) {
        ApiTraceCall *call = static_cast<ApiTraceCall*>(event);
        m_ui.detailsWebView->setHtml(call->toHtml());
        m_ui.detailsDock->show();
        if (call->hasBinaryData()) {
            QByteArray data =
                call->arguments()[call->binaryDataIndex()].toByteArray();
            m_vdataInterpreter->setData(data);
            QVariantList args = call->arguments();

            for (int i = 0; i < call->argNames().count(); ++i) {
                QString name = call->argNames()[i];
                if (name == QLatin1String("stride")) {
                    int stride = args[i].toInt();
                    m_ui.vertexStrideSB->setValue(stride);
                } else if (name == QLatin1String("size")) {
                    int components = args[i].toInt();
                    m_ui.vertexComponentsSB->setValue(components);
                } else if (name == QLatin1String("type")) {
                    QString val = args[i].toString();
                    int textIndex = m_ui.vertexTypeCB->findText(val);
                    if (textIndex >= 0)
                        m_ui.vertexTypeCB->setCurrentIndex(textIndex);
                }
            }
        }
        m_ui.vertexDataDock->setVisible(call->hasBinaryData());
        m_selectedEvent = call;
    } else {
        if (event && event->type() == ApiTraceEvent::Frame) {
            m_selectedEvent = static_cast<ApiTraceFrame*>(event);
        } else
            m_selectedEvent = 0;
        m_ui.detailsDock->hide();
        m_ui.vertexDataDock->hide();
    }
    if (m_selectedEvent && !m_selectedEvent->state().isEmpty()) {
        fillStateForFrame();
    } else
        m_ui.stateDock->hide();
}

void MainWindow::replayStart()
{
    if (m_trace->isSaving()) {
        QMessageBox::warning(
            this,
            tr("Trace Saving"),
            tr("QApiTrace is currently saving the edited trace file. "
               "Please wait until it finishes and try again."));
        return;
    }
    QDialog dlg;
    Ui_RetracerDialog dlgUi;
    dlgUi.setupUi(&dlg);

    dlgUi.doubleBufferingCB->setChecked(
        m_retracer->isDoubleBuffered());
    dlgUi.errorCheckCB->setChecked(
        !m_retracer->isBenchmarking());

    if (dlg.exec() == QDialog::Accepted) {
        m_retracer->setDoubleBuffered(
            dlgUi.doubleBufferingCB->isChecked());
        m_retracer->setBenchmarking(
            !dlgUi.errorCheckCB->isChecked());
        replayTrace(false);
    }
}

void MainWindow::replayStop()
{
    m_retracer->quit();
    m_ui.actionStop->setEnabled(false);
    m_ui.actionReplay->setEnabled(true);
    m_ui.actionLookupState->setEnabled(true);
}

void MainWindow::newTraceFile(const QString &fileName)
{
    qDebug()<< "Loading  : " <<fileName;

    m_progressBar->setValue(0);
    m_trace->setFileName(fileName);

    if (fileName.isEmpty()) {
        m_ui.actionReplay->setEnabled(false);
        m_ui.actionLookupState->setEnabled(false);
        setWindowTitle(tr("QApiTrace"));
    } else {
        QFileInfo info(fileName);
        m_ui.actionReplay->setEnabled(true);
        m_ui.actionLookupState->setEnabled(true);
        setWindowTitle(
            tr("QApiTrace - %1").arg(info.fileName()));
    }
}

void MainWindow::replayFinished(const QString &output)
{
    m_ui.actionStop->setEnabled(false);
    m_ui.actionReplay->setEnabled(true);
    m_ui.actionLookupState->setEnabled(true);

    m_progressBar->hide();
    if (output.length() < 80) {
        statusBar()->showMessage(output);
    }
    m_stateEvent = 0;
    m_ui.actionShowErrorsDock->setEnabled(m_trace->hasErrors());
    m_ui.errorsDock->setVisible(m_trace->hasErrors());
    if (!m_trace->hasErrors())
        m_ui.errorsTreeWidget->clear();

    statusBar()->showMessage(
        tr("Replaying finished!"), 2000);
}

void MainWindow::replayError(const QString &message)
{
    m_ui.actionStop->setEnabled(false);
    m_ui.actionReplay->setEnabled(true);
    m_ui.actionLookupState->setEnabled(true);
    m_stateEvent = 0;
    m_nonDefaultsLookupEvent = 0;

    m_progressBar->hide();
    statusBar()->showMessage(
        tr("Replaying unsuccessful."), 2000);
    QMessageBox::warning(
        this, tr("Replay Failed"), message);
}

void MainWindow::startedLoadingTrace()
{
    Q_ASSERT(m_trace);
    m_progressBar->show();
    QFileInfo info(m_trace->fileName());
    statusBar()->showMessage(
        tr("Loading %1...").arg(info.fileName()));
}

void MainWindow::finishedLoadingTrace()
{
    m_progressBar->hide();
    if (!m_trace) {
        return;
    }
    QFileInfo info(m_trace->fileName());
    statusBar()->showMessage(
        tr("Loaded %1").arg(info.fileName()), 3000);
}

void MainWindow::replayTrace(bool dumpState)
{
    if (m_trace->fileName().isEmpty())
        return;

    m_retracer->setFileName(m_trace->fileName());
    m_retracer->setCaptureState(dumpState);
    if (m_retracer->captureState() && m_selectedEvent) {
        int index = 0;
        if (m_selectedEvent->type() == ApiTraceEvent::Call) {
            index = static_cast<ApiTraceCall*>(m_selectedEvent)->index();
        } else if (m_selectedEvent->type() == ApiTraceEvent::Frame) {
            ApiTraceFrame *frame =
                static_cast<ApiTraceFrame*>(m_selectedEvent);
            if (frame->isEmpty()) {
                //XXX i guess we could still get the current state
                qDebug()<<"tried to get a state for an empty frame";
                return;
            }
            index = frame->calls().first()->index();
        } else {
            qDebug()<<"Unknown event type";
            return;
        }
        m_retracer->setCaptureAtCallNumber(index);
    }
    m_retracer->start();

    m_ui.actionStop->setEnabled(true);
    m_progressBar->show();
    if (dumpState)
        statusBar()->showMessage(
            tr("Looking up the state..."));
    else
        statusBar()->showMessage(
            tr("Replaying the trace file..."));
}

void MainWindow::lookupState()
{
    if (!m_selectedEvent) {
        QMessageBox::warning(
            this, tr("Unknown Event"),
            tr("To inspect the state select an event in the event list."));
        return;
    }
    if (m_trace->isSaving()) {
        QMessageBox::warning(
            this,
            tr("Trace Saving"),
            tr("QApiTrace is currently saving the edited trace file. "
               "Please wait until it finishes and try again."));
        return;
    }
    m_stateEvent = m_selectedEvent;
    replayTrace(true);
}

MainWindow::~MainWindow()
{
    delete m_trace;
    m_trace = 0;

    delete m_proxyModel;
    delete m_model;
}

static void
variantToString(const QVariant &var, QString &str)
{
    if (var.type() == QVariant::List) {
        QVariantList lst = var.toList();
        str += QLatin1String("[");
        for (int i = 0; i < lst.count(); ++i) {
            QVariant val = lst[i];
            variantToString(val, str);
            if (i < lst.count() - 1)
                str += QLatin1String(", ");
        }
        str += QLatin1String("]");
    } else if (var.type() == QVariant::Map) {
        Q_ASSERT(!"unsupported state type");
    } else if (var.type() == QVariant::Hash) {
        Q_ASSERT(!"unsupported state type");
    } else {
        str += var.toString();
    }
}

static QTreeWidgetItem *
variantToItem(const QString &key, const QVariant &var, const QVariant &defaultVar);

static void
variantMapToItems(const QVariantMap &map, const QVariantMap &defaultMap, QList<QTreeWidgetItem *> &items)
{
    QVariantMap::const_iterator itr;
    for (itr = map.constBegin(); itr != map.constEnd(); ++itr) {
        QString key = itr.key();
        QVariant var = itr.value();
        QVariant defaultVar = defaultMap[key];

        QTreeWidgetItem *item = variantToItem(key, var, defaultVar);
        if (item) {
            items.append(item);
        }
    }
}

static void
variantListToItems(const QVariantList &lst, const QVariantList &defaultLst, QList<QTreeWidgetItem *> &items)
{
    for (int i = 0; i < lst.count(); ++i) {
        QString key = QString::number(i);
        QVariant var = lst[i];
        QVariant defaultVar;
        
        if (i < defaultLst.count()) {
            defaultVar = defaultLst[i];
        }

        QTreeWidgetItem *item = variantToItem(key, var, defaultVar);
        if (item) {
            items.append(item);
        }
    }
}

static bool
isVariantDeep(const QVariant &var)
{
    if (var.type() == QVariant::List) {
        QVariantList lst = var.toList();
        for (int i = 0; i < lst.count(); ++i) {
            if (isVariantDeep(lst[i])) {
                return true;
            }
        }
        return false;
    } else if (var.type() == QVariant::Map) {
        return true;
    } else if (var.type() == QVariant::Hash) {
        return true;
    } else {
        return false;
    }
}

static QTreeWidgetItem *
variantToItem(const QString &key, const QVariant &var, const QVariant &defaultVar)
{
    if (var == defaultVar) {
        return NULL;
    }

    QString val;

    bool deep = isVariantDeep(var);
    if (!deep) {
        variantToString(var, val);
    }

    //qDebug()<<"key = "<<key;
    //qDebug()<<"val = "<<val;
    QStringList lst;
    lst += key;
    lst += val;

    QTreeWidgetItem *item = new QTreeWidgetItem((QTreeWidgetItem *)0, lst);

    if (deep) {
        QList<QTreeWidgetItem *> children;
        if (var.type() == QVariant::Map) {
            QVariantMap map = var.toMap();
            QVariantMap defaultMap = defaultVar.toMap();
            variantMapToItems(map, defaultMap, children);
        }
        if (var.type() == QVariant::List) {
            QVariantList lst = var.toList();
            QVariantList defaultLst = defaultVar.toList();
            variantListToItems(lst, defaultLst, children);
        }
        item->addChildren(children);
    }

    return item;
}

void MainWindow::fillStateForFrame()
{
    QVariantMap params;

    if (!m_selectedEvent || m_selectedEvent->state().isEmpty())
        return;

    if (m_nonDefaultsLookupEvent) {
        m_ui.nonDefaultsCB->blockSignals(true);
        m_ui.nonDefaultsCB->setChecked(true);
        m_ui.nonDefaultsCB->blockSignals(false);
    }

    bool nonDefaults = m_ui.nonDefaultsCB->isChecked();
    QVariantMap defaultParams;
    if (nonDefaults) {
        ApiTraceState defaultState = m_trace->defaultState();
        defaultParams = defaultState.parameters();
    }

    const ApiTraceState &state = m_selectedEvent->state();
    m_ui.stateTreeWidget->clear();
    params = state.parameters();
    QList<QTreeWidgetItem *> items;
    variantMapToItems(params, defaultParams, items);
    m_ui.stateTreeWidget->insertTopLevelItems(0, items);

    QMap<QString, QString> shaderSources = state.shaderSources();
    if (shaderSources.isEmpty()) {
        m_sourcesWidget->setShaders(shaderSources);
    } else {
        m_sourcesWidget->setShaders(shaderSources);
    }

    const QList<ApiTexture> &textures =
        state.textures();
    const QList<ApiFramebuffer> &fbos =
        state.framebuffers();

    m_ui.surfacesTreeWidget->clear();
    if (textures.isEmpty() && fbos.isEmpty()) {
        m_ui.surfacesTab->setDisabled(false);
    } else {
        m_ui.surfacesTreeWidget->setIconSize(QSize(64, 64));
        if (!textures.isEmpty()) {
            QTreeWidgetItem *textureItem =
                new QTreeWidgetItem(m_ui.surfacesTreeWidget);
            textureItem->setText(0, tr("Textures"));
            if (textures.count() <= 6)
                textureItem->setExpanded(true);

            for (int i = 0; i < textures.count(); ++i) {
                const ApiTexture &texture =
                    textures[i];
                QIcon icon(QPixmap::fromImage(texture.thumb()));
                QTreeWidgetItem *item = new QTreeWidgetItem(textureItem);
                item->setIcon(0, icon);
                int width = texture.size().width();
                int height = texture.size().height();
                QString descr =
                    QString::fromLatin1("%1, %2 x %3")
                    .arg(texture.target())
                    .arg(width)
                    .arg(height);
                item->setText(1, descr);

                item->setData(0, Qt::UserRole,
                              texture.image());
            }
        }
        if (!fbos.isEmpty()) {
            QTreeWidgetItem *fboItem =
                new QTreeWidgetItem(m_ui.surfacesTreeWidget);
            fboItem->setText(0, tr("Framebuffers"));
            if (fbos.count() <= 6)
                fboItem->setExpanded(true);

            for (int i = 0; i < fbos.count(); ++i) {
                const ApiFramebuffer &fbo =
                    fbos[i];
                QIcon icon(QPixmap::fromImage(fbo.thumb()));
                QTreeWidgetItem *item = new QTreeWidgetItem(fboItem);
                item->setIcon(0, icon);
                int width = fbo.size().width();
                int height = fbo.size().height();
                QString descr =
                    QString::fromLatin1("%1, %2 x %3")
                    .arg(fbo.type())
                    .arg(width)
                    .arg(height);
                item->setText(1, descr);

                item->setData(0, Qt::UserRole,
                              fbo.image());
            }
        }
        m_ui.surfacesTab->setEnabled(true);
    }
    m_ui.stateDock->show();
}

void MainWindow::showSettings()
{
    SettingsDialog dialog;
    dialog.setFilterModel(m_proxyModel);

    dialog.exec();
}

void MainWindow::openHelp(const QUrl &url)
{
    QDesktopServices::openUrl(url);
}

void MainWindow::showSurfacesMenu(const QPoint &pos)
{
    QTreeWidget *tree = m_ui.surfacesTreeWidget;
    QTreeWidgetItem *item = tree->itemAt(pos);
    if (!item)
        return;

    QMenu menu(tr("Surfaces"), this);
    //add needed actions
    QAction *act = menu.addAction(tr("View Image"));
    act->setStatusTip(tr("View the currently selected surface"));
    connect(act, SIGNAL(triggered()),
            SLOT(showSelectedSurface()));

    menu.exec(tree->viewport()->mapToGlobal(pos));
}

void MainWindow::showSelectedSurface()
{
    QTreeWidgetItem *item =
        m_ui.surfacesTreeWidget->currentItem();

    if (!item)
        return;

    QVariant var = item->data(0, Qt::UserRole);
    QImage img = var.value<QImage>();
    ImageViewer *viewer = new ImageViewer(this);

    QString title;
    if (currentCall()) {
        title = tr("QApiTrace - Surface at %1 (%2)")
                .arg(currentCall()->name())
                .arg(currentCall()->index());
    } else {
        title = tr("QApiTrace - Surface Viewer");
    }
    viewer->setWindowTitle(title);
    viewer->setAttribute(Qt::WA_DeleteOnClose, true);
    viewer->setImage(img);
    QRect screenRect = QApplication::desktop()->availableGeometry();
    viewer->resize(qMin(int(0.75 * screenRect.width()), img.width()) + 40,
                   qMin(int(0.75 * screenRect.height()), img.height()) + 40);
    viewer->show();
    viewer->raise();
    viewer->activateWindow();
}

void MainWindow::initObjects()
{
    m_ui.stateTreeWidget->sortByColumn(0, Qt::AscendingOrder);

    m_sourcesWidget = new ShadersSourceWidget(m_ui.shadersTab);
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(m_sourcesWidget);
    m_ui.shadersTab->setLayout(layout);

    m_trace = new ApiTrace();
    m_retracer = new Retracer(this);

    m_vdataInterpreter = new VertexDataInterpreter(this);
    m_vdataInterpreter->setListWidget(m_ui.vertexDataListWidget);
    m_vdataInterpreter->setStride(
        m_ui.vertexStrideSB->value());
    m_vdataInterpreter->setComponents(
        m_ui.vertexComponentsSB->value());
    m_vdataInterpreter->setStartingOffset(
        m_ui.startingOffsetSB->value());
    m_vdataInterpreter->setTypeFromString(
        m_ui.vertexTypeCB->currentText());

    m_model = new ApiTraceModel();
    m_model->setApiTrace(m_trace);
    m_proxyModel = new ApiTraceFilter();
    m_proxyModel->setSourceModel(m_model);
    m_ui.callView->setModel(m_proxyModel);
    m_ui.callView->setItemDelegate(
        new ApiCallDelegate(m_ui.callView));
    m_ui.callView->resizeColumnToContents(0);
    m_ui.callView->header()->swapSections(0, 1);
    m_ui.callView->setColumnWidth(1, 42);
    m_ui.callView->setContextMenuPolicy(Qt::CustomContextMenu);

    m_progressBar = new QProgressBar();
    m_progressBar->setRange(0, 0);
    statusBar()->addPermanentWidget(m_progressBar);
    m_progressBar->hide();

    m_argsEditor = new ArgumentsEditor(this);

    m_ui.detailsDock->hide();
    m_ui.errorsDock->hide();
    m_ui.vertexDataDock->hide();
    m_ui.stateDock->hide();
    setDockOptions(dockOptions() | QMainWindow::ForceTabbedDocks);

    tabifyDockWidget(m_ui.stateDock, m_ui.vertexDataDock);
    tabifyDockWidget(m_ui.detailsDock, m_ui.errorsDock);

    m_ui.surfacesTreeWidget->setContextMenuPolicy(Qt::CustomContextMenu);

    m_ui.detailsWebView->page()->setLinkDelegationPolicy(
        QWebPage::DelegateExternalLinks);

    m_jumpWidget = new JumpWidget(this);
    m_ui.centralLayout->addWidget(m_jumpWidget);
    m_jumpWidget->hide();

    m_searchWidget = new SearchWidget(this);
    m_ui.centralLayout->addWidget(m_searchWidget);
    m_searchWidget->hide();

    m_traceProcess = new TraceProcess(this);
}

void MainWindow::initConnections()
{
    connect(m_trace, SIGNAL(startedLoadingTrace()),
            this, SLOT(startedLoadingTrace()));
    connect(m_trace, SIGNAL(finishedLoadingTrace()),
            this, SLOT(finishedLoadingTrace()));
    connect(m_trace, SIGNAL(startedSaving()),
            this, SLOT(slotStartedSaving()));
    connect(m_trace, SIGNAL(saved()),
            this, SLOT(slotSaved()));
    connect(m_trace, SIGNAL(changed(ApiTraceCall*)),
            this, SLOT(slotTraceChanged(ApiTraceCall*)));

    connect(m_retracer, SIGNAL(finished(const QString&)),
            this, SLOT(replayFinished(const QString&)));
    connect(m_retracer, SIGNAL(error(const QString&)),
            this, SLOT(replayError(const QString&)));
    connect(m_retracer, SIGNAL(foundState(const ApiTraceState&)),
            this, SLOT(replayStateFound(const ApiTraceState&)));
    connect(m_retracer, SIGNAL(retraceErrors(const QList<RetraceError>&)),
            this, SLOT(slotRetraceErrors(const QList<RetraceError>&)));

    connect(m_ui.vertexInterpretButton, SIGNAL(clicked()),
            m_vdataInterpreter, SLOT(interpretData()));
    connect(m_ui.vertexTypeCB, SIGNAL(currentIndexChanged(const QString&)),
            m_vdataInterpreter, SLOT(setTypeFromString(const QString&)));
    connect(m_ui.vertexStrideSB, SIGNAL(valueChanged(int)),
            m_vdataInterpreter, SLOT(setStride(int)));
    connect(m_ui.vertexComponentsSB, SIGNAL(valueChanged(int)),
            m_vdataInterpreter, SLOT(setComponents(int)));
    connect(m_ui.startingOffsetSB, SIGNAL(valueChanged(int)),
            m_vdataInterpreter, SLOT(setStartingOffset(int)));


    connect(m_ui.actionNew, SIGNAL(triggered()),
            this, SLOT(createTrace()));
    connect(m_ui.actionOpen, SIGNAL(triggered()),
            this, SLOT(openTrace()));
    connect(m_ui.actionQuit, SIGNAL(triggered()),
            this, SLOT(close()));

    connect(m_ui.actionFind, SIGNAL(triggered()),
            this, SLOT(slotSearch()));
    connect(m_ui.actionGo, SIGNAL(triggered()),
            this, SLOT(slotGoTo()));
    connect(m_ui.actionGoFrameStart, SIGNAL(triggered()),
            this, SLOT(slotGoFrameStart()));
    connect(m_ui.actionGoFrameEnd, SIGNAL(triggered()),
            this, SLOT(slotGoFrameEnd()));

    connect(m_ui.actionReplay, SIGNAL(triggered()),
            this, SLOT(replayStart()));
    connect(m_ui.actionStop, SIGNAL(triggered()),
            this, SLOT(replayStop()));
    connect(m_ui.actionLookupState, SIGNAL(triggered()),
            this, SLOT(lookupState()));
    connect(m_ui.actionOptions, SIGNAL(triggered()),
            this, SLOT(showSettings()));

    connect(m_ui.callView, SIGNAL(activated(const QModelIndex &)),
            this, SLOT(callItemSelected(const QModelIndex &)));
    connect(m_ui.callView, SIGNAL(customContextMenuRequested(QPoint)),
            this, SLOT(customContextMenuRequested(QPoint)));

    connect(m_ui.surfacesTreeWidget,
            SIGNAL(customContextMenuRequested(const QPoint &)),
            SLOT(showSurfacesMenu(const QPoint &)));
    connect(m_ui.surfacesTreeWidget,
            SIGNAL(itemDoubleClicked(QTreeWidgetItem *, int)),
            SLOT(showSelectedSurface()));

    connect(m_ui.detailsWebView, SIGNAL(linkClicked(const QUrl&)),
            this, SLOT(openHelp(const QUrl&)));

    connect(m_ui.nonDefaultsCB, SIGNAL(toggled(bool)),
            this, SLOT(fillState(bool)));

    connect(m_jumpWidget, SIGNAL(jumpTo(int)),
            SLOT(slotJumpTo(int)));

    connect(m_searchWidget,
            SIGNAL(searchNext(const QString&, Qt::CaseSensitivity)),
            SLOT(slotSearchNext(const QString&, Qt::CaseSensitivity)));
    connect(m_searchWidget,
            SIGNAL(searchPrev(const QString&, Qt::CaseSensitivity)),
            SLOT(slotSearchPrev(const QString&, Qt::CaseSensitivity)));

    connect(m_traceProcess, SIGNAL(tracedFile(const QString&)),
            SLOT(createdTrace(const QString&)));
    connect(m_traceProcess, SIGNAL(error(const QString&)),
            SLOT(traceError(const QString&)));

    connect(m_ui.errorsDock, SIGNAL(visibilityChanged(bool)),
            m_ui.actionShowErrorsDock, SLOT(setChecked(bool)));
    connect(m_ui.actionShowErrorsDock, SIGNAL(triggered(bool)),
            m_ui.errorsDock, SLOT(setVisible(bool)));
    connect(m_ui.errorsTreeWidget,
            SIGNAL(currentItemChanged(QTreeWidgetItem*, QTreeWidgetItem*)),
            this, SLOT(slotErrorSelected(QTreeWidgetItem*)));
}

void MainWindow::replayStateFound(const ApiTraceState &state)
{
    m_stateEvent->setState(state);
    m_model->stateSetOnEvent(m_stateEvent);
    if (m_selectedEvent == m_stateEvent ||
        m_nonDefaultsLookupEvent == m_selectedEvent) {
        fillStateForFrame();
    } else {
        m_ui.stateDock->hide();
    }
    m_nonDefaultsLookupEvent = 0;
}

void MainWindow::slotGoTo()
{
    m_searchWidget->hide();
    m_jumpWidget->show();
}

void MainWindow::slotJumpTo(int callNum)
{
    QModelIndex index = m_proxyModel->callIndex(callNum);
    if (index.isValid()) {
        m_ui.callView->setCurrentIndex(index);
    }
}

void MainWindow::createdTrace(const QString &path)
{
    qDebug()<<"Done tracing "<<path;
    newTraceFile(path);
}

void MainWindow::traceError(const QString &msg)
{
    QMessageBox::warning(
            this,
            tr("Tracing Error"),
            msg);
}

void MainWindow::slotSearch()
{
    m_jumpWidget->hide();
    m_searchWidget->show();
}

void MainWindow::slotSearchNext(const QString &str,
                                Qt::CaseSensitivity sensitivity)
{
    QModelIndex index = m_ui.callView->currentIndex();
    ApiTraceEvent *event = 0;


    if (!index.isValid()) {
        index = m_proxyModel->index(0, 0, QModelIndex());
        if (!index.isValid()) {
            qDebug()<<"no currently valid index";
            m_searchWidget->setFound(false);
            return;
        }
    }

    event = index.data(ApiTraceModel::EventRole).value<ApiTraceEvent*>();
    ApiTraceCall *call = 0;

    if (event->type() == ApiTraceCall::Call)
        call = static_cast<ApiTraceCall*>(event);
    else {
        Q_ASSERT(event->type() == ApiTraceCall::Frame);
        ApiTraceFrame *frame = static_cast<ApiTraceFrame*>(event);
        call = frame->call(0);
    }

    if (!call) {
        m_searchWidget->setFound(false);
        return;
    }
    const QList<ApiTraceCall*> &calls = m_trace->calls();
    int callNum = calls.indexOf(call);

    for (int i = callNum + 1; i < calls.count(); ++i) {
        ApiTraceCall *testCall = calls[i];
        QModelIndex index = m_proxyModel->indexForCall(testCall);
        /* if it's not valid it means that the proxy model has already
         * filtered it out */
        if (index.isValid()) {
            QString txt = testCall->filterText();
            if (txt.contains(str, sensitivity)) {
                m_ui.callView->setCurrentIndex(index);
                m_searchWidget->setFound(true);
                return;
            }
        }
    }
    m_searchWidget->setFound(false);
}

void MainWindow::slotSearchPrev(const QString &str,
                                Qt::CaseSensitivity sensitivity)
{
    QModelIndex index = m_ui.callView->currentIndex();
    ApiTraceEvent *event = 0;


    if (!index.isValid()) {
        index = m_proxyModel->index(0, 0, QModelIndex());
        if (!index.isValid()) {
            qDebug()<<"no currently valid index";
            m_searchWidget->setFound(false);
            return;
        }
    }

    event = index.data(ApiTraceModel::EventRole).value<ApiTraceEvent*>();
    ApiTraceCall *call = 0;

    if (event->type() == ApiTraceCall::Call)
        call = static_cast<ApiTraceCall*>(event);
    else {
        Q_ASSERT(event->type() == ApiTraceCall::Frame);
        ApiTraceFrame *frame = static_cast<ApiTraceFrame*>(event);
        call = frame->call(0);
    }

    if (!call) {
        m_searchWidget->setFound(false);
        return;
    }
    const QList<ApiTraceCall*> &calls = m_trace->calls();
    int callNum = calls.indexOf(call);

    for (int i = callNum - 1; i >= 0; --i) {
        ApiTraceCall *testCall = calls[i];
        QModelIndex index = m_proxyModel->indexForCall(testCall);
        /* if it's not valid it means that the proxy model has already
         * filtered it out */
        if (index.isValid()) {
            QString txt = testCall->filterText();
            if (txt.contains(str, sensitivity)) {
                m_ui.callView->setCurrentIndex(index);
                m_searchWidget->setFound(true);
                return;
            }
        }
    }
    m_searchWidget->setFound(false);
}

void MainWindow::fillState(bool nonDefaults)
{
    if (nonDefaults) {
        ApiTraceState defaultState = m_trace->defaultState();
        if (defaultState.isEmpty()) {
            m_ui.nonDefaultsCB->blockSignals(true);
            m_ui.nonDefaultsCB->setChecked(false);
            m_ui.nonDefaultsCB->blockSignals(false);
            int ret = QMessageBox::question(
                this, tr("Empty Default State"),
                tr("The applcation needs to figure out the "
                   "default state for the current trace. "
                   "This only has to be done once and "
                   "afterwards you will be able to enable "
                   "displaying of non default state for all calls."
                   "\nDo you want to lookup the default state now?"),
                QMessageBox::Yes | QMessageBox::No);
            if (ret != QMessageBox::Yes)
                return;
            ApiTraceFrame *firstFrame =
                m_trace->frameAt(0);
            ApiTraceEvent *oldSelected = m_selectedEvent;
            if (!firstFrame)
                return;
            m_nonDefaultsLookupEvent = m_selectedEvent;
            m_selectedEvent = firstFrame;
            lookupState();
            m_selectedEvent = oldSelected;
        }
    }
    fillStateForFrame();
}

void MainWindow::customContextMenuRequested(QPoint pos)
{
    QMenu menu;
    QModelIndex index = m_ui.callView->indexAt(pos);

    callItemSelected(index);
    if (!index.isValid())
        return;

    ApiTraceEvent *event =
        index.data(ApiTraceModel::EventRole).value<ApiTraceEvent*>();
    if (!event || event->type() != ApiTraceEvent::Call)
        return;

    menu.addAction(QIcon(":/resources/media-record.png"),
                   tr("Lookup state"), this, SLOT(lookupState()));
    menu.addAction(tr("Edit"), this, SLOT(editCall()));

    menu.exec(QCursor::pos());
}

void MainWindow::editCall()
{
    if (m_selectedEvent && m_selectedEvent->type() == ApiTraceEvent::Call) {
        ApiTraceCall *call = static_cast<ApiTraceCall*>(m_selectedEvent);
        m_argsEditor->setCall(call);
        m_argsEditor->show();
    }
}

void MainWindow::slotStartedSaving()
{
    m_progressBar->show();
    statusBar()->showMessage(
        tr("Saving to %1").arg(m_trace->fileName()));
}

void MainWindow::slotSaved()
{
    statusBar()->showMessage(
        tr("Saved to %1").arg(m_trace->fileName()), 2000);
    m_progressBar->hide();
}

void MainWindow::slotGoFrameStart()
{
    ApiTraceFrame *frame = currentFrame();
    if (!frame || frame->isEmpty()) {
        return;
    }

    QList<ApiTraceCall*>::const_iterator itr;
    QList<ApiTraceCall*> calls = frame->calls();

    itr = calls.constBegin();
    while (itr != calls.constEnd()) {
        ApiTraceCall *call = *itr;
        QModelIndex idx = m_proxyModel->indexForCall(call);
        if (idx.isValid()) {
            m_ui.callView->setCurrentIndex(idx);
            break;
        }
        ++itr;
    }
}

void MainWindow::slotGoFrameEnd()
{
    ApiTraceFrame *frame = currentFrame();
    if (!frame || frame->isEmpty()) {
        return;
    }
    QList<ApiTraceCall*>::const_iterator itr;
    QList<ApiTraceCall*> calls = frame->calls();

    itr = calls.constEnd();
    do {
        --itr;
        ApiTraceCall *call = *itr;
        QModelIndex idx = m_proxyModel->indexForCall(call);
        if (idx.isValid()) {
            m_ui.callView->setCurrentIndex(idx);
            break;
        }
    } while (itr != calls.constBegin());
}

ApiTraceFrame * MainWindow::currentFrame() const
{
    if (m_selectedEvent) {
        if (m_selectedEvent->type() == ApiTraceEvent::Frame) {
            return static_cast<ApiTraceFrame*>(m_selectedEvent);
        } else {
            Q_ASSERT(m_selectedEvent->type() == ApiTraceEvent::Call);
            ApiTraceCall *call = static_cast<ApiTraceCall*>(m_selectedEvent);
            return call->parentFrame();
        }
    }
    return NULL;
}

void MainWindow::slotTraceChanged(ApiTraceCall *call)
{
    Q_ASSERT(call);
    if (call == m_selectedEvent) {
        m_ui.detailsWebView->setHtml(call->toHtml());
    }
}

void MainWindow::slotRetraceErrors(const QList<RetraceError> &errors)
{
    m_ui.errorsTreeWidget->clear();

    foreach(RetraceError error, errors) {
        ApiTraceCall *call = m_trace->callWithIndex(error.callIndex);
        if (!call)
            continue;
        call->setError(error.message);

        QTreeWidgetItem *item =
            new QTreeWidgetItem(m_ui.errorsTreeWidget);
        item->setData(0, Qt::DisplayRole, error.callIndex);
        item->setData(0, Qt::UserRole, QVariant::fromValue(call));
        QString type = error.type;
        type[0] = type[0].toUpper();
        item->setData(1, Qt::DisplayRole, type);
        item->setData(2, Qt::DisplayRole, error.message);
    }
}

void MainWindow::slotErrorSelected(QTreeWidgetItem *current)
{
    if (current) {
        ApiTraceCall *call =
            current->data(0, Qt::UserRole).value<ApiTraceCall*>();
        Q_ASSERT(call);
        QModelIndex index = m_proxyModel->indexForCall(call);
        if (index.isValid()) {
            m_ui.callView->setCurrentIndex(index);
        } else {
            statusBar()->showMessage(tr("Call has been filtered out."));
        }
    }
}

ApiTraceCall * MainWindow::currentCall() const
{
    if (m_selectedEvent &&
        m_selectedEvent->type() == ApiTraceEvent::Call) {
        return static_cast<ApiTraceCall*>(m_selectedEvent);
    }
    return NULL;
}

#include "mainwindow.moc"
