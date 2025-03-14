/* This file is (c) 2008-2012 Konstantin Isakov <ikm@goldendict.org>
 * Part of GoldenDict. Licensed under GPLv3 or later, see the LICENSE file */

#ifndef NO_EPWING_SUPPORT
#include "epwing_book.hh"
#endif

#include "mainwindow.hh"
#include "editdictionaries.hh"
#include "loaddictionaries.hh"
#include "preferences.hh"
#include "about.hh"
#include "article_urlschemehandler.hh"
#include "mruqmenu.hh"
#include "gestures.hh"
#include "dictheadwords.hh"
#include <limits.h>
#include <QDebug>
#include <QTextStream>
#include <QDir>
#include <QUrl>
#include <QMessageBox>
#include <QIcon>
#include <QList>
#include <QSet>
#include <QToolBar>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QProcess>
#include <QFileDialog>
#include <QPrinter>
#include <QPageSetupDialog>
#include <QPrintPreviewDialog>
#include <QPrintDialog>
#include <QProgressDialog>
#include <QRunnable>
#include <QThreadPool>
#include <QSslConfiguration>

#include <limits.h>
#include "gddebug.hh"

#include "dictinfo.hh"
#include "fsencoding.hh"
#include "historypanewidget.hh"
#include "qt4x5.hh"
#include <QDesktopWidget>
#include "ui_authentication.h"

#ifndef USE_QTWEBKIT
#include <QPointer>
#include <QVersionNumber>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#endif

#ifdef Q_OS_MAC
#include "lionsupport.h"
#include "macmouseover.hh"
#endif

#ifdef Q_OS_WIN32
#include <windows.h>
#include "mouseover_win32/GDDataTranfer.h"
#include "wstring.hh"
#include "wstring_qt.hh"

#define gdStoreNormalGeometryEvent ( ( QEvent::Type )( QEvent::User + 1 ) )
#define gdApplyNormalGeometryEvent ( ( QEvent::Type )( QEvent::User + 2 ) )

#endif

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
#include <QX11Info>
#include <X11/Xlib.h>
#include <fixx11h.h>
#endif

#define MIN_THREAD_COUNT 4

using std::wstring;

namespace {

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
class MinimumSizeWidget: public QWidget
{
  Q_OBJECT
public:
  explicit MinimumSizeWidget( QWidget * parent ):
    QWidget( parent )
  {
    setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Fixed );
  }

  virtual QSize sizeHint() const
  { return QSize( 1, 1 ); }
};
#endif

#ifndef USE_QTWEBKIT
/// Both compile-time and run-time Qt WebEngine versions may and often do differ from the corresponding
/// Qt versions. That's because building Qt WebEngine with earlier Qt versions is officially supported.
/// Patch releases of Qt WebEngine are available as free software and are packaged by GNU/Linux
/// distributions a year before the patch releases of the corresponding LTS (e.g. 5.15.x) Qt versions.
QVersionNumber runTimeQtWebEngineVersion( QWebEngineProfile const & webEngineProfile )
{
#if QT_VERSION >= QT_VERSION_CHECK( 6, 2, 0 )
  Q_UNUSED( webEngineProfile )
  return QVersionNumber::fromString( QLatin1String{ qWebEngineVersion() } );
#else
  QString const httpUserAgent = webEngineProfile.httpUserAgent();
  // httpUserAgent should contain a substring like "QtWebEngine/5.15.10 ". The "5.15.10" part
  // equals the run-time version of Qt WebEngine. The code below extracts this version.

  QLatin1String const prefix( "QtWebEngine/" );
  auto webEngineVersionIndex = httpUserAgent.indexOf( prefix );
  if( webEngineVersionIndex == -1 )
  {
    gdWarning( "Failed to parse the Qt WebEngine profile's HTTP User-Agent string: %s",
               qUtf8Printable( httpUserAgent ) );
    return QVersionNumber();
  }
  webEngineVersionIndex += prefix.size();

  return QVersionNumber::fromString( QStringView{ httpUserAgent }.mid( webEngineVersionIndex ) );
#endif
}

StreamingDeviceWorkarounds computeStreamingDeviceWorkarounds( QWebEngineProfile const & webEngineProfile )
{
  auto const webEngineVersion = runTimeQtWebEngineVersion( webEngineProfile );
  if( webEngineVersion.isNull() )
    return StreamingDeviceWorkarounds::None; // This must be a future version => probably no need for the workarounds.

#if QT_VERSION >= QT_VERSION_CHECK( 6, 0, 0 )
  if( webEngineVersion >= QVersionNumber( 6, 4, 1 ) )
    return StreamingDeviceWorkarounds::None;
  if( webEngineVersion >= QVersionNumber( 6, 4, 0 ) )
    return StreamingDeviceWorkarounds::AtEndOnly;
#else
  if( webEngineVersion >= QVersionNumber( 5, 15, 11 ) )
    return StreamingDeviceWorkarounds::AtEndOnly;
#endif
  return StreamingDeviceWorkarounds::AtEndAndReadData;
}

void setWebEngineProfilePaths( QWebEngineProfile & webEngineProfile )
{
  Q_ASSERT( !webEngineProfile.isOffTheRecord() );

  QString cachePath = webEngineProfile.cachePath();
  if( Config::replaceWritableCacheLocationIn( cachePath ) )
    webEngineProfile.setCachePath( cachePath );

  QString persistentStoragePath = webEngineProfile.persistentStoragePath();
  if( Config::replaceWritableDataLocationIn( persistentStoragePath ) )
    webEngineProfile.setPersistentStoragePath( persistentStoragePath );
}

void setupWebEngineProfile( QWebEngineProfile & webEngineProfile, ArticleNetworkAccessManager & articleNetMgr )
{
  // TODO (Qt WebEngine): should the maximum size of the HTTP cache and whether
  // it is cleared on exit be configurable similarly to the network cache?

  // TODO (Qt WebEngine): in the Qt WebEngine version articleNetMgr's cache is useful only for downloaded files,
  // because the page itself is stored separately in the Qt WebEngine profile's cache. In the interest of
  // reusing the Qt WebEngine profile's cache for both loading pages and downloading files,
  // Dictionary::WebMultimediaDownload could call QWebEnginePage::download() instead of
  // QNetworkAccessManager::get() to retrieve files from network. articleNetMgr's cache would become
  // practically useless then and could be restricted to the Qt WebKit version along with its configuration UI.
  // QWebEnginePage::download() always stores the downloaded data in a file. A QNetworkReply returned by
  // QNetworkAccessManager::get() stores the downloaded data in a buffer. This cache reuse requires substantial
  // refactoring to avoid the overhead of unnecessary reading from and writing to a file, because the users of
  // WebMultimediaDownload access the data via the DataRequest interface and store it into a file, except for
  // pronouncing of an external audio link, which passes DataRequest::data to AudioPlayerInterface::play().
  // This refactoring should eliminate existing code duplication between ArticleView::resourceDownloadFinished()
  // and ResourceToSaveHandler::downloadFinished() as well as fix minor issues in these functions along the way.

  // TODO (Qt WebEngine): should the configuration UI allow disabling persistent cookies?
  // Cookies are never stored on disk in the Qt WebKit version according to the documentation for QNetworkCookieJar:
  // > QNetworkCookieJar does not implement permanent storage: it only keeps the cookies in memory. Once the
  // > QNetworkCookieJar object is deleted, all cookies it held will be discarded as well. If you want to save the
  // > cookies, you should derive from this class and implement the saving to disk to your own storage format.
  // Should the persistent cookies be disabled by default in the Qt WebEngine version too?

  articleNetMgr.setStreamingDeviceWorkarounds( computeStreamingDeviceWorkarounds( webEngineProfile ) );

  auto * const handler = new ArticleUrlSchemeHandler( articleNetMgr );
  handler->install( webEngineProfile );
}
#endif // USE_QTWEBKIT

} // unnamed namespace

#ifndef QT_NO_OPENSSL

class InitSSLRunnable : public QRunnable
{
  virtual void run()
  {
    /// This action force SSL library initialisation which may continue a few seconds
    QSslConfiguration::setDefaultConfiguration( QSslConfiguration::defaultConfiguration() );
  }
};

#endif

MainWindow::MainWindow( Config::Class & cfg_ ):
  trayIcon( 0 ),
  groupLabel( &searchPaneTitleBar ),
  foundInDictsLabel( &dictsPaneTitleBar ),
  escAction( this ),
  focusTranslateLineAction( this ),
  addTabAction( this ),
  closeCurrentTabAction( this ),
  closeAllTabAction( this ),
  closeRestTabAction( this ),
  switchToNextTabAction( this ),
  switchToPrevTabAction( this ),
  showDictBarNamesAction( tr( "Show Names in Dictionary &Bar" ), this ),
  useSmallIconsInToolbarsAction( tr( "Show Small Icons in &Toolbars" ), this ),
  toggleMenuBarAction( tr( "&Menubar" ), this ),
  switchExpandModeAction( this ),
  focusHeadwordsDlgAction( this ),
  focusArticleViewAction( this ),
  addAllTabToFavoritesAction( this ),
  trayIconMenu( this ),
  addTab( this ),
  cfg( cfg_ ),
  history( History::Load(), cfg_.preferences.maxStringsInHistory, cfg_.maxHeadwordSize ),
  dictionaryBar( this, configEvents, cfg.editDictionaryCommandLine, cfg.preferences.maxDictionaryRefsInContextMenu ),
  articleMaker( dictionaries, groupInstances, cfg.preferences.displayStyle, cfg.preferences.addonStyle, this ),
  articleNetMgr( this, dictionaries, articleMaker,
                 cfg.preferences.disallowContentFromOtherSites, cfg.preferences.hideGoldenDictHeader ),
  dictNetMgr( this ),
  audioPlayerFactory( cfg.preferences ),
  wordFinder( this ),
  newReleaseCheckTimer( this ),
  latestReleaseReply( 0 ),
  wordListSelChanged( false )
, wasMaximized( false )
, blockUpdateWindowTitle( false )
, headwordsDlg( 0 )
, ftsIndexing( dictionaries )
, ftsDlg( 0 )
, helpWindow( 0 )
, starIcon( ":/icons/star.png" )
, blueStarIcon( ":/icons/star_blue.png" )
#ifdef Q_OS_WIN32
, gdAskMessage( 0xFFFFFFFF )
#endif
{
  if( QThreadPool::globalInstance()->maxThreadCount() < MIN_THREAD_COUNT )
    QThreadPool::globalInstance()->setMaxThreadCount( MIN_THREAD_COUNT );

#ifndef QT_NO_OPENSSL
  QThreadPool::globalInstance()->start( new InitSSLRunnable );
#endif

  qRegisterMetaType< Config::InputPhrase >();

#ifndef NO_EPWING_SUPPORT
  Epwing::initialize();
#endif

  ui.setupUi( this );

  articleMaker.setCollapseParameters( cfg.preferences.collapseBigArticles, cfg.preferences.articleSizeLimit );

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  // Set own gesture recognizers
  Gestures::registerRecognizers();
#endif

  // use our own, custom statusbar
  setStatusBar(0);
  mainStatusBar = new MainStatusBar( this );

  // Make the toolbar
  navToolbar = addToolBar( tr( "&Navigation" ) );
  navToolbar->setObjectName( "navToolbar" );

  navBack = navToolbar->addAction( QIcon( ":/icons/previous.png" ), tr( "Back" ) );
  navToolbar->widgetForAction( navBack )->setObjectName( "backButton" );
  navForward = navToolbar->addAction( QIcon( ":/icons/next.png" ), tr( "Forward" ) );
  navToolbar->widgetForAction( navForward )->setObjectName( "forwardButton" );

  QWidget * translateBoxWidget = new QWidget( this );
  QHBoxLayout * translateBoxLayout = new QHBoxLayout( translateBoxWidget );
  translateBoxWidget->setLayout( translateBoxLayout );
  translateBoxLayout->setContentsMargins( 0, 0, 0, 0 );
  translateBoxLayout->setSpacing( 0 );

  // translate box
  groupListInToolbar = new GroupComboBox( navToolbar );
  groupListInToolbar->setSizePolicy( QSizePolicy::Fixed, QSizePolicy::Preferred );
  groupListInToolbar->setSizeAdjustPolicy( QComboBox::AdjustToContents );
  translateBoxLayout->addWidget( groupListInToolbar );

  translateBox = new TranslateBox( navToolbar );
  translateBox->setSizePolicy( QSizePolicy::MinimumExpanding, QSizePolicy::Fixed );
  translateBoxLayout->addWidget( translateBox );
  translateBoxToolBarAction = navToolbar->addWidget( translateBoxWidget );

  // scan popup
  beforeScanPopupSeparator = navToolbar->addSeparator();
  beforeScanPopupSeparator->setVisible( cfg.preferences.enableScanPopup );
  navToolbar->widgetForAction( beforeScanPopupSeparator )->setObjectName( "beforeScanPopupSeparator" );

  enableScanPopup = navToolbar->addAction( QIcon( ":/icons/wizard.png" ), tr( "Scan Popup" ) );
  enableScanPopup->setCheckable( true );
  enableScanPopup->setVisible( cfg.preferences.enableScanPopup );
  navToolbar->widgetForAction( enableScanPopup )->setObjectName( "scanPopupButton" );
  if ( cfg.preferences.enableScanPopup && cfg.preferences.startWithScanPopupOn )
    enableScanPopup->setChecked( true );

  connect( enableScanPopup, SIGNAL( toggled( bool ) ),
           this, SLOT( scanEnableToggled( bool ) ) );

  afterScanPopupSeparator = navToolbar->addSeparator();
  afterScanPopupSeparator->setVisible( cfg.preferences.enableScanPopup );
  navToolbar->widgetForAction( afterScanPopupSeparator )->setObjectName( "afterScanPopupSeparator" );

  // sound
  navPronounce = navToolbar->addAction( QIcon( ":/icons/playsound_full.png" ), tr( "Pronounce Word (Alt+S)" ) );
  navPronounce->setShortcut( QKeySequence( "Alt+S" ) );
  navPronounce->setEnabled( false );
  navToolbar->widgetForAction( navPronounce )->setObjectName( "soundButton" );

  connect( navPronounce, SIGNAL( triggered() ),
           this, SLOT( pronounce() ) );

  // zooming
  // named separator (to be able to hide it via CSS)
  navToolbar->widgetForAction( navToolbar->addSeparator() )->setObjectName( "separatorBeforeZoom" );

  zoomIn = navToolbar->addAction( QIcon( ":/icons/icon32_zoomin.png" ), tr( "Zoom In" ) );
  zoomIn->setShortcuts( QList< QKeySequence >() <<
                       QKeySequence::ZoomIn <<
                       QKeySequence( "Ctrl+=" ) );
  navToolbar->widgetForAction( zoomIn )->setObjectName( "zoomInButton" );

  zoomOut = navToolbar->addAction( QIcon( ":/icons/icon32_zoomout.png" ), tr( "Zoom Out" ) );
  zoomOut->setShortcut( QKeySequence::ZoomOut );
  navToolbar->widgetForAction( zoomOut )->setObjectName( "zoomOutButton" );

  zoomBase = navToolbar->addAction( QIcon( ":/icons/icon32_zoombase.png" ), tr( "Normal Size" ) );
  zoomBase->setShortcut( QKeySequence( "Ctrl+0" ) );
  navToolbar->widgetForAction( zoomBase )->setObjectName( "zoomBaseButton" );

  // named separator (to be able to hide it via CSS)
  navToolbar->widgetForAction( navToolbar->addSeparator() )->setObjectName( "separatorBeforeSave" );

  navToolbar->addAction( ui.saveArticle );
  navToolbar->widgetForAction( ui.saveArticle )->setObjectName( "saveArticleButton" );

  navToolbar->addAction( ui.print );
  navToolbar->widgetForAction( ui.print )->setObjectName( "printButton" );

  navToolbar->widgetForAction( navToolbar->addSeparator() )->setObjectName( "separatorBeforeAddToFavorites" );

  addToFavorites = navToolbar->addAction( starIcon, tr( "Add current tab to Favorites" ) );
  navToolbar->widgetForAction( addToFavorites )->setObjectName( "addToFavoritesButton" );

  connect( addToFavorites, SIGNAL( triggered() ), this, SLOT( handleAddToFavoritesButton() ) );
  connect( ui.actionAddToFavorites, SIGNAL( triggered() ), this, SLOT( addCurrentTabToFavorites() ) );

  beforeOptionsSeparator = navToolbar->addSeparator();
  navToolbar->widgetForAction( beforeOptionsSeparator )->setObjectName( "beforeOptionsSeparator" );
  beforeOptionsSeparator->setVisible( cfg.preferences.hideMenubar);

  QMenu * buttonMenu = new QMenu( this );
  buttonMenu->addAction( ui.dictionaries );
  buttonMenu->addAction( ui.preferences );
  buttonMenu->addSeparator();
  buttonMenu->addMenu( ui.menuFavorites );
  buttonMenu->addMenu( ui.menuHistory );
  buttonMenu->addSeparator();
  buttonMenu->addMenu( ui.menuFile );
  buttonMenu->addMenu( ui.menuView );
  buttonMenu->addMenu( ui.menuSearch );
  buttonMenu->addMenu( ui.menu_Help );

  ui.fullTextSearchAction->setEnabled( cfg.preferences.fts.enabled );

  menuButton = new QToolButton( navToolbar );
  menuButton->setPopupMode( QToolButton::InstantPopup );
  menuButton->setMenu( buttonMenu );
  menuButton->setIcon( QIcon (":/icons/menu_button.png") );
  menuButton->addAction( ui.menuOptions );
  menuButton->setToolTip( tr( "Menu Button" ) );
  menuButton->setObjectName( "menuButton" );
  menuButton->setFocusPolicy( Qt::NoFocus );

  menuButtonAction = navToolbar->addWidget(menuButton);
  menuButtonAction->setVisible( cfg.preferences.hideMenubar );

  // Make the search pane's titlebar
  groupLabel.setText( tr( "Look up in:" ) );
  groupListInDock = new GroupComboBox( &searchPaneTitleBar );

  searchPaneTitleBarLayout.setContentsMargins( 8, 5, 8, 4 );
  searchPaneTitleBarLayout.addWidget( &groupLabel );
  searchPaneTitleBarLayout.addWidget( groupListInDock );
  searchPaneTitleBarLayout.addStretch();

  searchPaneTitleBar.setLayout( &searchPaneTitleBarLayout );

  ui.searchPane->setTitleBarWidget( &searchPaneTitleBar );
  connect( ui.searchPane->toggleViewAction(), SIGNAL( triggered( bool ) ),
           this, SLOT( updateSearchPaneAndBar( bool ) ) );

  // Bind the loading indicator's height to the translate line's height to prevent UI shifts on loading state changes.
  ui.loadingIndicatorLabel->setSameHeightWidget( ui.translateLine );

  if ( cfg.preferences.searchInDock )
  {
    groupList = groupListInDock;
    translateLine = ui.translateLine;
    wordList = ui.wordList;
  }
  else
  {
    groupList = groupListInToolbar;
    translateLine = translateBox->translateLine();
    wordList = translateBox->wordList();
  }
  wordList->attachFinder( &wordFinder );

  // for the old UI:
  ui.wordList->setTranslateLine( ui.translateLine );

  groupList->setFocusPolicy(Qt::ClickFocus);
  wordList->setFocusPolicy(Qt::ClickFocus);

  wordListDefaultFont = wordList->font();
  translateLineDefaultFont = translateLine->font();
  groupListDefaultFont = groupList->font();

  // Make the dictionaries pane's titlebar
  foundInDictsLabel.setText( tr( "Found in Dictionaries:" ) );
  dictsPaneTitleBarLayout.addWidget( &foundInDictsLabel );
  dictsPaneTitleBarLayout.setContentsMargins(5, 5, 5, 5);
  dictsPaneTitleBar.setLayout( &dictsPaneTitleBarLayout );
  dictsPaneTitleBar.setObjectName("dictsPaneTitleBar");
  ui.dictsPane->setTitleBarWidget( &dictsPaneTitleBar );
  ui.dictsList->setContextMenuPolicy( Qt::CustomContextMenu );

  connect( ui.dictsPane, SIGNAL( visibilityChanged( bool ) ),
           this, SLOT( dictsPaneVisibilityChanged ( bool ) ) );

  connect( ui.dictsList, SIGNAL( itemClicked( QListWidgetItem * ) ),
           this, SLOT( foundDictsPaneClicked( QListWidgetItem * ) ) );

  connect( ui.dictsList, SIGNAL( customContextMenuRequested( const QPoint & ) ),
           this, SLOT( foundDictsContextMenuRequested( const QPoint & ) ) );

  connect( zoomIn, SIGNAL( triggered() ),
           this, SLOT( zoomin() ) );
  connect( zoomOut, SIGNAL( triggered() ),
           this, SLOT( zoomout() ) );
  connect( zoomBase, SIGNAL( triggered() ),
           this, SLOT( unzoom() ) );

  ui.menuZoom->addAction( zoomIn );
  ui.menuZoom->addAction( zoomOut );
  ui.menuZoom->addAction( zoomBase );

  ui.menuZoom->addSeparator();

  wordsZoomIn = ui.menuZoom->addAction( QIcon( ":/icons/icon32_zoomin.png" ), tr( "Words Zoom In" ) );
  wordsZoomIn->setShortcuts( QList< QKeySequence >() <<
                            QKeySequence( "Alt++" ) <<
                            QKeySequence( "Alt+=" ) );
  wordsZoomOut = ui.menuZoom->addAction( QIcon( ":/icons/icon32_zoomout.png" ), tr( "Words Zoom Out" ) );
  wordsZoomOut->setShortcut( QKeySequence( "Alt+-" ) );
  wordsZoomBase = ui.menuZoom->addAction( QIcon( ":/icons/icon32_zoombase.png" ), tr( "Words Normal Size" ) );
  wordsZoomBase->setShortcut( QKeySequence( "Alt+0" ) );

  connect( wordsZoomIn, SIGNAL(triggered()), this, SLOT(doWordsZoomIn()) );
  connect( wordsZoomOut, SIGNAL(triggered()), this, SLOT(doWordsZoomOut()) );
  connect( wordsZoomBase, SIGNAL(triggered()), this, SLOT(doWordsZoomBase()) );

  // tray icon
  connect( trayIconMenu.addAction( tr( "Show &Main Window" ) ), SIGNAL( triggered() ),
           this, SLOT( showMainWindow() ) );
  trayIconMenu.addAction( enableScanPopup );
  trayIconMenu.addSeparator();
  connect( trayIconMenu.addAction( tr( "&Quit" ) ), SIGNAL( triggered() ),
           this, SLOT( quitApp() ) );

  addGlobalAction( &escAction, SLOT( handleEsc() ) );
  escAction.setShortcut( QKeySequence( "Esc" ) );

  addGlobalAction( &focusTranslateLineAction, SLOT( focusTranslateLine() ) );
  focusTranslateLineAction.setShortcuts( QList< QKeySequence >() <<
                                         QKeySequence( "Alt+D" ) <<
                                         QKeySequence( "Ctrl+L" ) );

  addGlobalAction( &focusHeadwordsDlgAction, SLOT( focusHeadwordsDialog() ) );
  focusHeadwordsDlgAction.setShortcut( QKeySequence( "Ctrl+D" ) );

  addGlobalAction( &focusArticleViewAction, SLOT( focusArticleView() ) );
  focusArticleViewAction.setShortcut( QKeySequence( "Ctrl+N" ) );

  addGlobalAction( ui.fullTextSearchAction, SLOT( showFullTextSearchDialog() ) );

  addTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  addTabAction.setShortcut( QKeySequence( "Ctrl+T" ) );

  // Tab management
  tabListMenu = new MRUQMenu(tr("Opened tabs"), ui.tabWidget);

  connect (tabListMenu, SIGNAL(ctrlReleased()), this, SLOT(ctrlReleased()));

  connect( &addTabAction, SIGNAL( triggered() ),
           this, SLOT( addNewTab() ) );

  addAction( &addTabAction );

  closeCurrentTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  closeCurrentTabAction.setShortcut( QKeySequence( "Ctrl+W" ) );
  closeCurrentTabAction.setText( tr("Close current tab") );
  closeCurrentTabAction.setIcon( QIcon(":/icons/closetab.png") );

  connect( &closeCurrentTabAction, SIGNAL( triggered() ),
           this, SLOT( closeCurrentTab() ) );

  addAction( &closeCurrentTabAction );

  closeAllTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  closeAllTabAction.setShortcut( QKeySequence( "Ctrl+Shift+W" ) );
  closeAllTabAction.setText( tr("Close all tabs") );

  connect( &closeAllTabAction, SIGNAL( triggered() ),
           this, SLOT( closeAllTabs() ) );

  addAction( &closeAllTabAction );

  closeRestTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  closeRestTabAction.setText( tr("Close all tabs except current") );

  connect( &closeRestTabAction, SIGNAL( triggered() ),
           this, SLOT( closeRestTabs() ) );

  addAction( &closeRestTabAction );

  switchToNextTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  switchToNextTabAction.setShortcut( QKeySequence( "Ctrl+PgDown" ) );

  connect( &switchToNextTabAction, SIGNAL( triggered() ),
           this, SLOT( switchToNextTab() ) );

  addAction( &switchToNextTabAction );

  switchToPrevTabAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  switchToPrevTabAction.setShortcut( QKeySequence( "Ctrl+PgUp" ) );

  connect( &switchToPrevTabAction, SIGNAL( triggered() ),
           this, SLOT( switchToPrevTab() ) );

  addAction( &switchToPrevTabAction );

  switchExpandModeAction.setShortcutContext( Qt::WidgetWithChildrenShortcut );
  switchExpandModeAction.setShortcuts( QList< QKeySequence >() <<
                                       QKeySequence( Qt::CTRL + Qt::Key_8 ) <<
                                       QKeySequence( Qt::CTRL + Qt::Key_Asterisk ) <<
                                       QKeySequence( Qt::CTRL + Qt::SHIFT + Qt::Key_8 ) );

  connect( &switchExpandModeAction, SIGNAL( triggered() ),
           this, SLOT(switchExpandOptionalPartsMode() ) );

  addAction( &switchExpandModeAction );

  addAllTabToFavoritesAction.setText( tr( "Add all tabs to Favorites" ) );

  connect( &addAllTabToFavoritesAction, SIGNAL( triggered() ),
           this, SLOT( addAllTabsToFavorites() ) );

  tabMenu = new QMenu(this);
  tabMenu->addAction( &closeCurrentTabAction );
  tabMenu->addAction( &closeRestTabAction );
  tabMenu->addSeparator();
  tabMenu->addAction( &closeAllTabAction );
  tabMenu->addSeparator();
  tabMenu->addAction( addToFavorites );
  tabMenu->addAction( &addAllTabToFavoritesAction );

  // Dictionary bar names

  showDictBarNamesAction.setCheckable( true );
  showDictBarNamesAction.setChecked( cfg.showingDictBarNames );

  connect( &showDictBarNamesAction, SIGNAL( triggered() ),
           this, SLOT( showDictBarNamesTriggered() ) );

  // Use small icons in toolbars

  useSmallIconsInToolbarsAction.setCheckable( true );
  useSmallIconsInToolbarsAction.setChecked( cfg.usingSmallIconsInToolbars );

  connect( &useSmallIconsInToolbarsAction, SIGNAL( triggered() ),
           this, SLOT( useSmallIconsInToolbarsTriggered() ) );

  // Toggle Menubar
  toggleMenuBarAction.setCheckable( true );
  toggleMenuBarAction.setChecked( !cfg.preferences.hideMenubar );
  toggleMenuBarAction.setShortcut( QKeySequence( "Ctrl+M" ) );

  connect( &toggleMenuBarAction, SIGNAL( triggered() ),
           this, SLOT( toggleMenuBarTriggered() ) );

  // Populate 'View' menu

  ui.menuView->addAction( &toggleMenuBarAction );
  ui.menuView->addSeparator();
  ui.menuView->addAction( ui.searchPane->toggleViewAction() );
  ui.searchPane->toggleViewAction()->setShortcut( QKeySequence( "Ctrl+S" ) );
  ui.menuView->addAction( ui.dictsPane->toggleViewAction() );
  ui.dictsPane->toggleViewAction()->setShortcut( QKeySequence( "Ctrl+R" ) );
  ui.menuView->addAction( ui.favoritesPane->toggleViewAction() );
  ui.favoritesPane->toggleViewAction()->setShortcut( QKeySequence( "Ctrl+I" ) );
  ui.menuView->addAction( ui.historyPane->toggleViewAction() );
  ui.historyPane->toggleViewAction()->setShortcut( QKeySequence( "Ctrl+H" ) );
  ui.menuView->addSeparator();
  ui.menuView->addAction( dictionaryBar.toggleViewAction() );
  ui.menuView->addAction( navToolbar->toggleViewAction() );
  ui.menuView->addSeparator();
  ui.menuView->addAction( &showDictBarNamesAction );
  ui.menuView->addAction( &useSmallIconsInToolbarsAction );
  ui.menuView->addSeparator();
  ui.alwaysOnTop->setChecked( cfg.preferences.alwaysOnTop );
  ui.menuView->addAction( ui.alwaysOnTop );

  // Dictionary bar

  Instances::Group const * igrp = groupInstances.findGroup( cfg.lastMainGroupId );
  if( cfg.lastMainGroupId == Instances::Group::AllGroupId )
  {
    if( igrp )
      igrp->checkMutedDictionaries( &cfg.mutedDictionaries );
    dictionaryBar.setMutedDictionaries( &cfg.mutedDictionaries );
  }
  else
  {
    Config::Group * grp = cfg.getGroup( cfg.lastMainGroupId );
    if( igrp && grp )
      igrp->checkMutedDictionaries( &grp->mutedDictionaries );
    dictionaryBar.setMutedDictionaries( grp ? &grp->mutedDictionaries : 0 );
  }

  showDictBarNamesTriggered(); // Make update its state according to initial
                               // setting

  useSmallIconsInToolbarsTriggered();

  connect( this, SIGNAL( clickOnDictPane( QString const & ) ),
           &dictionaryBar, SLOT( dictsPaneClicked( QString const & ) ) );

  addToolBar( &dictionaryBar );

  connect( dictionaryBar.toggleViewAction(), SIGNAL(triggered(bool)),
           this, SLOT(dictionaryBarToggled(bool)) );
  // This one will be disconnected once the slot is activated. It exists
  // only to handle the initial appearance of the dictionary bar.
  connect( dictionaryBar.toggleViewAction(), SIGNAL(toggled(bool)),
           this, SLOT(dictionaryBarToggled(bool)) );

  connect( &dictionaryBar, SIGNAL(editGroupRequested()),
           this, SLOT(editCurrentGroup()) );

  connect( &dictionaryBar, SIGNAL( showDictionaryInfo( QString const & ) ),
           this, SLOT( showDictionaryInfo( QString const & ) ) );

  connect( &dictionaryBar, SIGNAL( showDictionaryHeadwords( QString const & ) ),
           this, SLOT( showDictionaryHeadwords( QString const & ) ) );

  connect( &dictionaryBar, SIGNAL( openDictionaryFolder( QString const & ) ),
           this, SLOT( openDictionaryFolder( QString const & ) ) );

  // Favorites

  ui.favoritesPaneWidget->setUp( &cfg, ui.menuFavorites );
  ui.favoritesPaneWidget->setSaveInterval( cfg.preferences.favoritesStoreInterval );

  connect( ui.favoritesPane, SIGNAL( visibilityChanged( bool ) ),
           this, SLOT( updateFavoritesMenu() ) );

  connect( ui.menuFavorites, SIGNAL( aboutToShow() ),
           this, SLOT( updateFavoritesMenu() ) );

  connect( ui.favoritesPaneWidget, SIGNAL( favoritesItemRequested( QString, QString ) ),
           this, SLOT( headwordFromFavorites( QString, QString ) ) );

  // History
  ui.historyPaneWidget->setUp( &cfg, &history, ui.menuHistory );
  history.enableAdd( cfg.preferences.storeHistory );

  connect( ui.historyPaneWidget, SIGNAL( historyItemRequested( QString const & ) ),
           this, SLOT( showHistoryItem( QString const & ) ) );

  connect( ui.historyPane, SIGNAL( visibilityChanged( bool ) ),
           this, SLOT( updateHistoryMenu() ) );

  connect( ui.menuHistory, SIGNAL( aboutToShow() ),
           this, SLOT( updateHistoryMenu() ) );

#if !defined( HAVE_X11 ) || QT_VERSION < QT_VERSION_CHECK( 5, 0, 0 )
  // Show tray icon early so the user would be happy. It won't be functional
  // though until the program inits fully.
  // Do not create dummy tray icon in X. Cause QT5 failed to upgrade systemtray context menu.
  // And as result menu for some DEs apppear to be empty, for example in MATE DE.

  if ( cfg.preferences.enableTrayIcon )
  {
    trayIcon = new QSystemTrayIcon( QIcon::fromTheme("goldendict-tray", QIcon( ":/icons/programicon_old.png" )), this );
    trayIcon->setToolTip( tr( "Loading..." ) );
    trayIcon->show();
  }
#endif

  connect( navBack, SIGNAL( triggered() ),
           this, SLOT( backClicked() ) );
  connect( navForward, SIGNAL( triggered() ),
           this, SLOT( forwardClicked() ) );

  addTab.setAutoRaise( true );
  addTab.setToolTip( tr( "New Tab"  ) );
  addTab.setFocusPolicy( Qt::NoFocus );
  addTab.setIcon( QIcon( ":/icons/addtab.png" ) );

  ui.tabWidget->setHideSingleTab(cfg.preferences.hideSingleTab);
  ui.tabWidget->clear();

  ui.tabWidget->setCornerWidget( &addTab, Qt::TopLeftCorner );
  //ui.tabWidget->setCornerWidget( &closeTab, Qt::TopRightCorner );

#if QT_VERSION >= 0x040500
  ui.tabWidget->setMovable( true );
#endif

#ifndef Q_OS_WIN32
  ui.tabWidget->setDocumentMode( true );
#endif

  ui.tabWidget->setContextMenuPolicy( Qt::CustomContextMenu );

  connect( &addTab, SIGNAL( clicked() ),
           this, SLOT( addNewTab() ) );

  connect( ui.tabWidget, SIGNAL( doubleClicked() ),
           this, SLOT( addNewTab() ) );

  connect( ui.tabWidget, SIGNAL( tabCloseRequested( int ) ),
           this, SLOT( tabCloseRequested( int ) ) );

  connect( ui.tabWidget, SIGNAL( currentChanged( int ) ),
           this, SLOT( tabSwitched( int ) ) );

  connect( ui.tabWidget, SIGNAL( customContextMenuRequested(QPoint)) ,
           this, SLOT( tabMenuRequested(QPoint)) );

#if QT_VERSION >= 0x040500
  ui.tabWidget->setTabsClosable( true );
#endif

  connect( ui.quit, SIGNAL( triggered() ),
           this, SLOT( quitApp() ) );

  connect( ui.dictionaries, SIGNAL( triggered() ),
           this, SLOT( editDictionaries() ) );

  connect( ui.preferences, SIGNAL( triggered() ),
           this, SLOT( editPreferences() ) );

  connect( ui.visitHomepage, SIGNAL( triggered() ),
           this, SLOT( visitHomepage() ) );
  connect( ui.visitForum, SIGNAL( triggered() ),
           this, SLOT( visitForum() ) );
  connect( ui.openConfigFolder, SIGNAL( triggered() ),
           this, SLOT( openConfigFolder() ) );
  connect( ui.about, SIGNAL( triggered() ),
           this, SLOT( showAbout() ) );
  connect( ui.showReference, SIGNAL( triggered() ),
           this, SLOT( showGDHelp() ) );

  connect( groupListInDock, SIGNAL( currentIndexChanged( QString const & ) ),
           this, SLOT( currentGroupChanged( QString const & ) ) );

  connect( groupListInToolbar, SIGNAL( currentIndexChanged( QString const & ) ),
           this, SLOT( currentGroupChanged( QString const & ) ) );

  connect( ui.translateLine, SIGNAL( textChanged( QString const & ) ),
           this, SLOT( translateInputChanged( QString const & ) ) );

  connect( translateBox->translateLine(), SIGNAL( textChanged( QString const & ) ),
           this, SLOT( translateInputChanged( QString const & ) ) );

  connect( ui.translateLine, SIGNAL( returnPressed() ),
           this, SLOT( translateInputFinished() ) );

  connect( translateBox->translateLine(), SIGNAL( returnPressed() ),
           this, SLOT( translateInputFinished() ) );

  connect( ui.wordList, SIGNAL( itemSelectionChanged() ),
           this, SLOT( wordListSelectionChanged() ) );

  connect( translateBox->wordList(), SIGNAL( itemDoubleClicked ( QListWidgetItem * ) ),
           this, SLOT( wordListItemActivated( QListWidgetItem * ) ) );

  connect( ui.wordList, SIGNAL( itemClicked( QListWidgetItem * ) ),
           this, SLOT( wordListItemActivated( QListWidgetItem * ) ) );

  connect( ui.wordList, SIGNAL( statusBarMessage( QString const &, int, QPixmap const & ) ),
           this, SLOT( showStatusBarMessage( QString const &, int, QPixmap const & ) ) );

  connect( translateBox->wordList(), SIGNAL( statusBarMessage( QString const &, int, QPixmap const & ) ),
           this, SLOT( showStatusBarMessage( QString const &, int, QPixmap const & ) ) );

  connect( ui.dictsList, SIGNAL( itemSelectionChanged() ),
           this, SLOT( dictsListSelectionChanged() ) );

  connect( ui.dictsList, SIGNAL( itemDoubleClicked( QListWidgetItem * ) ),
           this, SLOT( dictsListItemActivated( QListWidgetItem * ) ) );

  connect( &configEvents, SIGNAL( mutedDictionariesChanged() ),
           this, SLOT( mutedDictionariesChanged() ) );

  this->installEventFilter( this );

  ui.translateLine->installEventFilter( this );
  translateBox->translateLine()->installEventFilter( this );

  ui.wordList->installEventFilter( this );
  translateBox->wordList()->installEventFilter( this );

  ui.wordList->viewport()->installEventFilter( this );
  translateBox->wordList()->viewport()->installEventFilter( this );

  ui.dictsList->installEventFilter( this );
  ui.dictsList->viewport()->installEventFilter( this );
  //tabWidget doesn't propagate Ctrl+Tab to the parent widget unless event filter is installed
  ui.tabWidget->installEventFilter( this );

  ui.historyList->installEventFilter( this );

  ui.favoritesTree->installEventFilter( this );

  groupListInDock->installEventFilter( this );
  groupListInToolbar->installEventFilter( this );

  connect( &ftsIndexing, SIGNAL( newIndexingName( QString ) ), this, SLOT( showFTSIndexingName( QString ) ) );

#ifdef Q_OS_WIN
  if( cfg.normalMainWindowGeometry.width() <= 0 )
  {
    QRect r = QApplication::desktop()->availableGeometry();
    cfg.normalMainWindowGeometry.setRect( r.width() / 4, r.height() / 4, r.width() / 2, r.height() / 2 );
  }
  if( cfg.maximizedMainWindowGeometry.width() > 0 )
  {
    setGeometry( cfg.maximizedMainWindowGeometry );
    if ( cfg.mainWindowGeometry.size() )
      restoreGeometry( cfg.mainWindowGeometry );
    if ( cfg.mainWindowState.size() )
      restoreState( cfg.mainWindowState, 1 );
    setWindowState( windowState() | Qt::WindowMaximized );
  }
  else
#endif
#ifndef Q_OS_MAC
  {
    if ( cfg.mainWindowGeometry.size() )
      restoreGeometry( cfg.mainWindowGeometry );
    if ( cfg.mainWindowState.size() )
      restoreState( cfg.mainWindowState, 1 );
  }
#endif
  updateSearchPaneAndBar( cfg.preferences.searchInDock );
  ui.searchPane->setVisible( cfg.preferences.searchInDock );

  applyProxySettings();

  connect( &dictNetMgr, SIGNAL( proxyAuthenticationRequired( QNetworkProxy, QAuthenticator * ) ),
           this, SLOT( proxyAuthentication( QNetworkProxy, QAuthenticator * ) ) );

  connect( &articleNetMgr, SIGNAL( proxyAuthenticationRequired( QNetworkProxy, QAuthenticator * ) ),
           this, SLOT( proxyAuthentication( QNetworkProxy, QAuthenticator * ) ) );

  setupNetworkCache( cfg.preferences.maxNetworkCacheSize );

#ifndef USE_QTWEBKIT
  if( cfg.preferences.offTheRecordWebProfile )
    webEngineProfile.reset( new QWebEngineProfile{} );
  else
  {
    webEngineProfile.reset( new QWebEngineProfile{ QStringLiteral( "Article" ) } );
    setWebEngineProfilePaths( *webEngineProfile );
  }
  setupWebEngineProfile( *webEngineProfile, articleNetMgr );
  ArticleView::initProfilePreferences( *webEngineProfile, cfg.preferences );
#endif

  applyWebSettings();

  makeDictionaries();

  // After we have dictionaries and groups, we can populate history
//  historyChanged();

  setWindowTitle( "GoldenDict" );

#ifdef Q_OS_MAC
  {
    if ( cfg.mainWindowGeometry.size() )
      restoreGeometry( cfg.mainWindowGeometry );
    if ( cfg.mainWindowState.size() )
      restoreState( cfg.mainWindowState, 1 );
  }
#endif

  blockUpdateWindowTitle = true;
  addNewTab();

  // Create tab list menu
  createTabList();

  // Show the initial welcome text

  {
    ArticleView *view = getCurrentArticleView();

    history.enableAdd( false );

    blockUpdateWindowTitle = true;

    view->showDefinition( tr( "Welcome!" ), Instances::Group::HelpGroupId );

    history.enableAdd( cfg.preferences.storeHistory );
  }

  translateLine->setFocus();

  applyQtStyleSheet( cfg.preferences.displayStyle, cfg.preferences.addonStyle );

  makeScanPopup();

  if ( trayIcon )
  {
    // Upgrade existing dummy tray icon into a full-functional one

    trayIcon->setContextMenu( &trayIconMenu );
    trayIcon->show();

    connect( trayIcon, SIGNAL( activated( QSystemTrayIcon::ActivationReason ) ),
             this, SLOT( trayIconActivated( QSystemTrayIcon::ActivationReason ) ) );
  }

  updateTrayIcon();

  // Update zoomers
  adjustCurrentZoomFactor();
  scaleArticlesByCurrentZoomFactor();
  applyWordsZoomLevel();

  // Update autostart info
  setAutostart(cfg.preferences.autoStart);

  // Initialize global hotkeys
  installHotKeys();

  if ( cfg.preferences.alwaysOnTop )
  {
    on_alwaysOnTop_triggered( true );
  }

  // Only show window initially if it wasn't configured differently
  if ( !cfg.preferences.enableTrayIcon || !cfg.preferences.startToTray )
  {
    show();
    focusTranslateLine();
  }

  connect( &newReleaseCheckTimer, SIGNAL( timeout() ),
           this, SLOT( checkForNewRelease() ) );

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
  // The X11 focus workaround in toggleMainWindow() emulates a left mouse button
  // click on the top-left pixel of the main GoldenDict window. This hack steals
  // focus from other applications reliably, but it also performs this click at
  // the position that belongs to the File menu in the default KDE Plasma style,
  // because the menu bar has no left margin there.
  // Insert a minimum-size widget, which ignores mouse clicks, to the left of the
  // menu bar to work around opening of the File menu each time the main window
  // is shown (e.g. via a hotkey or when another GoldenDict instance is launched).
  menuBar()->setCornerWidget( new MinimumSizeWidget( this ), Qt::TopLeftCorner );
#endif

  if ( cfg.preferences.hideMenubar )
  {
    toggleMenuBarTriggered( false );
  }

  prepareNewReleaseChecks();

  // makeDictionaries() didn't do deferred init - we do it here, at the end.
  doDeferredInit( dictionaries );

  updateStatusLine();

#ifdef Q_OS_MAC
  if( cfg.preferences.startWithScanPopupOn && !MacMouseOver::isAXAPIEnabled() )
      mainStatusBar->showMessage( tr( "Accessibility API is not enabled" ), 10000,
                                      QPixmap( ":/icons/error.png" ) );
#endif

  wasMaximized = isMaximized();

  history.setSaveInterval( cfg.preferences.historyStoreInterval );

  #ifdef Q_OS_MAC
    LionSupport::addFullscreen(this);
  #endif
#ifdef Q_OS_WIN32
  gdAskMessage = RegisterWindowMessage( GD_MESSAGE_NAME );
  ( static_cast< QHotkeyApplication * >( qApp ) )->setMainWindow( this );
#endif

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  ui.centralWidget->grabGesture( Gestures::GDPinchGestureType );
  ui.centralWidget->grabGesture( Gestures::GDSwipeGestureType );
#endif

  if( layoutDirection() == Qt::RightToLeft )
  {
    // Adjust button icons for Right-To-Left layout
    navBack->setIcon( QIcon( ":/icons/next.png" ) );
    navForward->setIcon( QIcon( ":/icons/previous.png" ) );
  }
}

void MainWindow::ctrlTabPressed()
{
    emit fillWindowsMenu();
    tabListButton->click();
}

void MainWindow::updateSearchPaneAndBar( bool searchInDock )
{
  QString text = translateLine->text();

  if( ftsDlg )
    removeGroupComboBoxActionsFromDialog( ftsDlg, groupList );
  if( headwordsDlg )
    removeGroupComboBoxActionsFromDialog( headwordsDlg, groupList );

  if ( searchInDock )
  {
    cfg.preferences.searchInDock = true;

    navToolbar->setAllowedAreas( Qt::AllToolBarAreas );

    groupList = groupListInDock;
    translateLine = ui.translateLine;
    wordList = ui.wordList;

    translateBoxToolBarAction->setVisible( false );
  }
  else
  {
    cfg.preferences.searchInDock = false;

    // handle the main toolbar, it must not be on the side, since it should
    // contain the group widget and the translate line. Valid locations: Top and Bottom.
    navToolbar->setAllowedAreas( Qt::BottomToolBarArea | Qt::TopToolBarArea );
    if ( toolBarArea( navToolbar ) & ( Qt::LeftToolBarArea | Qt::RightToolBarArea ) )
    {
      if ( toolBarArea( &dictionaryBar ) == Qt::TopToolBarArea )
      {
        insertToolBar( &dictionaryBar, navToolbar );
      }
      else
      {
        addToolBar( Qt::TopToolBarArea, navToolbar );
      }
    }

    groupList = groupListInToolbar;
    translateLine = translateBox->translateLine();
    wordList = translateBox->wordList();

    translateBoxToolBarAction->setVisible( true );
  }

  if( ftsDlg )
    addGroupComboBoxActionsToDialog( ftsDlg, groupList );
  if( headwordsDlg )
    addGroupComboBoxActionsToDialog( headwordsDlg, groupList );

  translateLine->setToolTip( tr( "String to search in dictionaries. The wildcards '*', '?' and sets of symbols '[...]' are allowed.\nTo find '*', '?', '[', ']' symbols use '\\*', '\\?', '\\[', '\\]' respectively" ) );

  // reset the flag when switching UI modes
  wordListSelChanged = false;

  wordList->attachFinder( &wordFinder );

  updateGroupList();
  applyWordsZoomLevel();
  updateIsPageLoading();

  setTranslateBoxTextAndKeepSuffix( text, WildcardsAreAlreadyEscaped, DisablePopup );
  focusTranslateLine();
}

void MainWindow::mousePressEvent( QMouseEvent *event)
{

  if (handleBackForwardMouseButtons( event ) )
  {
    return;
  }

  if (event->button() != Qt::MidButton)
    return QMainWindow::mousePressEvent(event);

  // middle clicked
  QString subtype = "plain";

    QString str = QApplication::clipboard()->text(subtype,
      QClipboard::Selection);
  setTranslateBoxTextAndClearSuffix( str, EscapeWildcards, NoPopupChange );

        QKeyEvent ev(QEvent::KeyPress, Qt::Key_Enter,
           Qt::NoModifier);
        QApplication::sendEvent(translateLine, &ev);
}

MainWindow::~MainWindow()
{
#ifdef Q_OS_WIN
  if( isMaximized() )
  {
    cfg.maximizedMainWindowGeometry = geometry();
  }
  else
  {
    cfg.maximizedMainWindowGeometry = QRect();
    if( !isMinimized() )
      cfg.normalMainWindowGeometry = geometry();
  }
#endif

  closeHeadwordsDialog();

  ftsIndexing.stopIndexing();

#if QT_VERSION >= QT_VERSION_CHECK(4, 6, 0)
  ui.centralWidget->ungrabGesture( Gestures::GDPinchGestureType );
  ui.centralWidget->ungrabGesture( Gestures::GDSwipeGestureType );
//  Gestures::unregisterRecognizers();
#endif

  // Close all tabs -- they should be destroyed before network managers
  // do.
  while( ui.tabWidget->count() )
  {
    QWidget * w = ui.tabWidget->widget( 0 );

    ui.tabWidget->removeTab( 0 );

    delete w;
  }

#ifndef NO_EPWING_SUPPORT
  Epwing::finalize();
#endif
}

void MainWindow::addGlobalAction( QAction * action, const char * slot )
{
  action->setShortcutContext( Qt::WidgetWithChildrenShortcut );
  connect( action, SIGNAL( triggered() ), this, slot );

  ui.centralWidget->addAction( action );
  ui.dictsPane->addAction( action );
  ui.searchPaneWidget->addAction( action );
  ui.favoritesPane->addAction( action );
  ui.historyPane->addAction( action );
  groupList->addAction( action );
  translateBox->addAction( action );
}

void MainWindow::addGlobalActionsToDialog( QDialog * dialog )
{
  dialog->addAction( &focusTranslateLineAction );
  dialog->addAction( &focusHeadwordsDlgAction );
  dialog->addAction( &focusArticleViewAction );
  dialog->addAction( ui.fullTextSearchAction );
}

void MainWindow::addGroupComboBoxActionsToDialog( QDialog * dialog, GroupComboBox * pGroupComboBox )
{
  dialog->addActions( pGroupComboBox->getExternActions() );
}

void MainWindow::removeGroupComboBoxActionsFromDialog( QDialog * dialog, GroupComboBox * pGroupComboBox )
{
  QList< QAction * > actions = pGroupComboBox->getExternActions();
  for( QList< QAction * >::iterator it = actions.begin(); it != actions.end(); ++it )
    dialog->removeAction( *it );
}

void MainWindow::commitData( QSessionManager & )
{
  commitData();
}

void MainWindow::commitData()
{
  if( cfg.preferences.clearNetworkCacheOnExit )
    if( QAbstractNetworkCache * cache = articleNetMgr.cache() )
      cache->clear();

  try
  {
    // Save MainWindow state and geometry
    cfg.mainWindowState = saveState( 1 );
    cfg.mainWindowGeometry = saveGeometry();

    // Save popup window state and geometry

    if( scanPopup.get() )
      scanPopup->saveConfigData();

    for( int i = 0, count = ui.tabWidget->count(); i < count; ++i )
      qobject_cast< ArticleView const * >( ui.tabWidget->widget( i ) )->saveConfigData();

    // Save any changes in last chosen groups etc
    try
    {
      Config::save( cfg );
    }
    catch( std::exception & e )
    {
      gdWarning( "Configuration saving failed, error: %s\n", e.what() );
    }

    // Save history
    history.save();

    // Save favorites
    ui.favoritesPaneWidget->saveData();
  }
  catch( std::exception & e )
  {
    gdWarning( "Commit data failed, error: %s\n", e.what() );
  }
}

QPrinter & MainWindow::getPrinter()
{
  if ( printer.get() )
    return *printer;

  printer = new QPrinter( QPrinter::HighResolution );

  return *printer;
}

void MainWindow::applyQtStyleSheet( QString const & displayStyle, QString const & addonStyle )
{
  QFile builtInCssFile( ":/qt-style.css" );
  builtInCssFile.open( QFile::ReadOnly );
  QByteArray css = builtInCssFile.readAll();

#if defined(Q_OS_MAC)
  QFile macCssFile( ":/qt-style-macos.css" );
  macCssFile.open( QFile::ReadOnly );
  css += macCssFile.readAll();
#endif

  if ( displayStyle.size() )
  {
    // Load an additional stylesheet
    QFile builtInCssFile( QString( ":/qt-style-st-%1.css" ).arg( displayStyle ) );
    if ( builtInCssFile.open( QFile::ReadOnly ) )
      css += builtInCssFile.readAll();
  }

  // Try loading a style sheet if there's one
  QFile cssFile( Config::getUserQtCssFileName() );

  if ( cssFile.open( QFile::ReadOnly ) )
    css += cssFile.readAll();

  if( !addonStyle.isEmpty() )
  {
    QString name = Config::getStylesDir() + addonStyle
                   + QDir::separator() + "qt-style.css";
    QFile addonCss( name );
    if( addonCss.open( QFile::ReadOnly ) )
      css += addonCss.readAll();
  }

  setStyleSheet( css );
}

void MainWindow::updateTrayIcon()
{
  if ( !trayIcon && cfg.preferences.enableTrayIcon )
  {
    // Need to show it
    trayIcon = new QSystemTrayIcon( QIcon::fromTheme("goldendict-tray", QIcon( ":/icons/programicon_old.png" )), this );
    trayIcon->setContextMenu( &trayIconMenu );
    trayIcon->show();

    connect( trayIcon, SIGNAL( activated( QSystemTrayIcon::ActivationReason ) ),
             this, SLOT( trayIconActivated( QSystemTrayIcon::ActivationReason ) ) );
  }
  else
  if ( trayIcon && !cfg.preferences.enableTrayIcon )
  {
    // Need to hide it
    delete trayIcon;

    trayIcon = 0;
  }
  if ( trayIcon )
  {
    // Update the icon to reflect the scanning mode
    trayIcon->setIcon( enableScanPopup->isChecked() ?
        QIcon::fromTheme("goldendict-scan-tray", QIcon( ":/icons/programicon_scan.png" )) :
        QIcon::fromTheme("goldendict-tray", QIcon( ":/icons/programicon_old.png" )) );

    trayIcon->setToolTip( "GoldenDict" );
  }

  // The 'Close to tray' action is associated with the tray icon, so we hide
  // or show it here.
  ui.actionCloseToTray->setVisible( cfg.preferences.enableTrayIcon );
}

void MainWindow::wheelEvent( QWheelEvent *ev )
{
  if ( ev->modifiers().testFlag( Qt::ControlModifier ) )
  {
    if ( ev->delta() > 0 )
    {
        zoomin();
    }
    else if ( ev->delta() < 0 )
    {
        zoomout();
    }
    ev->accept();
  }
  else
  {
    ev->ignore();
  }
}

void MainWindow::closeEvent( QCloseEvent * ev )
{
  if ( cfg.preferences.enableTrayIcon && cfg.preferences.closeToTray )
  {
    if( !cfg.preferences.searchInDock )
      translateBox->setPopupEnabled( false );

#ifdef HAVE_X11
    // Don't ignore the close event, because doing so cancels session logout if
    // the main window is visible when the user attempts to log out.
    // The main window will be only hidden, because QApplication::quitOnLastWindowClosed
    // property is false and Qt::WA_DeleteOnClose widget attribute is not set.
    Q_ASSERT(!QApplication::quitOnLastWindowClosed());
    Q_ASSERT(!testAttribute(Qt::WA_DeleteOnClose));
#else
    // Ignore the close event because closing the main window breaks global hotkeys on Windows.
    ev->ignore();
    hide();
#endif
  }
  else
  {
    ev->accept();
    quitApp();
  }
}

void MainWindow::quitApp()
{
  commitData();
  qApp->quit();
}

void MainWindow::applyProxySettings()
{
  if( cfg.preferences.proxyServer.enabled && cfg.preferences.proxyServer.useSystemProxy )
  {
    QList<QNetworkProxy> proxies = QNetworkProxyFactory::systemProxyForQuery();
    if( !cfg.preferences.proxyServer.systemProxyUser.isEmpty() )
    {
      proxies.first().setUser( cfg.preferences.proxyServer.systemProxyUser );
      proxies.first().setPassword( cfg.preferences.proxyServer.systemProxyPassword );
    }
    QNetworkProxy::setApplicationProxy( proxies.first() );
    return;
  }

  QNetworkProxy::ProxyType type = QNetworkProxy::NoProxy;

  if ( cfg.preferences.proxyServer.enabled )
  {
    switch( cfg.preferences.proxyServer.type )
    {
      case Config::ProxyServer::Socks5:
        type = QNetworkProxy::Socks5Proxy;
      break;
      case Config::ProxyServer::HttpConnect:
        type = QNetworkProxy::HttpProxy;
      break;
      case Config::ProxyServer::HttpGet:
        type = QNetworkProxy::HttpCachingProxy;
      break;

      default:
      break;
    }
  }

  QNetworkProxy proxy( type );

  if ( cfg.preferences.proxyServer.enabled )
  {
   proxy.setHostName( cfg.preferences.proxyServer.host );
   proxy.setPort( cfg.preferences.proxyServer.port );

   if ( cfg.preferences.proxyServer.user.size() )
     proxy.setUser( cfg.preferences.proxyServer.user );

   if ( cfg.preferences.proxyServer.password.size() )
     proxy.setPassword( cfg.preferences.proxyServer.password );
  }

  QNetworkProxy::setApplicationProxy( proxy );
}

void MainWindow::applyWebSettings()
{
#ifdef USE_QTWEBKIT
  QWebSettings *defaultSettings = QWebSettings::globalSettings();
  defaultSettings->setAttribute(QWebSettings::PluginsEnabled, cfg.preferences.enableWebPlugins);
  defaultSettings->setAttribute( QWebSettings::DeveloperExtrasEnabled, true );
#else
  auto * const settings = webEngineProfile->settings();
  settings->setAttribute( QWebEngineSettings::PluginsEnabled, cfg.preferences.enableWebPlugins );
#endif
}

void MainWindow::setupNetworkCache( int maxSize )
{
  // x << 20 == x * 2^20 converts mebibytes to bytes.
  qint64 const maxCacheSizeInBytes = maxSize <= 0 ? qint64( 0 ) : static_cast< qint64 >( maxSize ) << 20;

  if( QAbstractNetworkCache * abstractCache = articleNetMgr.cache() )
  {
    QNetworkDiskCache * const diskCache = qobject_cast< QNetworkDiskCache * >( abstractCache );
    Q_ASSERT_X( diskCache, Q_FUNC_INFO, "Unexpected network cache type." );
    diskCache->setMaximumCacheSize( maxCacheSizeInBytes );
    return;
  }
  if( maxCacheSizeInBytes == 0 )
    return; // There is currently no cache and it is not needed.

  QString const cacheDirectory = Config::getNetworkCacheDir();
  if( !QDir().mkpath( cacheDirectory ) )
  {
    gdWarning( "Cannot create a cache directory %s. Disabling network cache.", cacheDirectory.toUtf8().constData() );
    return;
  }
  QNetworkDiskCache * const diskCache = new QNetworkDiskCache( this );
  diskCache->setMaximumCacheSize( maxCacheSizeInBytes );
  diskCache->setCacheDirectory( cacheDirectory );
  articleNetMgr.setCache( diskCache );
}

void MainWindow::makeDictionaries()
{
  Q_ASSERT( !scanPopup && "Scan popup must not exist while dictionaries are initialized. "
                          "It does not support dictionaries changes and must be constructed anew." );

  wordFinder.clear();

  dictionariesUnmuted.clear();

  ftsIndexing.stopIndexing();
  ftsIndexing.clearDictionaries();

  loadDictionaries( this, isVisible(), cfg, dictionaries, dictNetMgr, false );

  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    dictionaries[ x ]->setFTSParameters( cfg.preferences.fts );
    dictionaries[ x ]->setSynonymSearchEnabled( cfg.preferences.synonymSearchEnabled );
  }

  ftsIndexing.setDictionaries( dictionaries );
  ftsIndexing.doIndexing();

  updateStatusLine();
  updateGroupList();
}

void MainWindow::updateStatusLine()
{
  unsigned articleCount = 0, wordCount = 0;

  for( unsigned x = dictionaries.size(); x--; )
  {
    articleCount += dictionaries[ x ]->getArticleCount();
    wordCount += dictionaries[ x ]->getWordCount();
  }

  mainStatusBar->showMessage( tr( "%1 dictionaries, %2 articles, %3 words" ).
                              arg( dictionaries.size() ).arg( articleCount ).
                              arg( wordCount ), 10000 );
}

void MainWindow::updateGroupList()
{
  bool haveGroups = cfg.groups.size();

  groupList->setVisible( haveGroups );

  groupLabel.setText( haveGroups ? tr( "Look up in:" ) : tr( "Look up:" ) );

  // currentIndexChanged() signal is very trigger-happy. To avoid triggering
  // it, we disconnect it while we're clearing and filling back groups.
  disconnect( groupList, SIGNAL( currentIndexChanged( QString const & ) ),
              this, SLOT( currentGroupChanged( QString const & ) ) );

  groupInstances.clear();

  // Add dictionaryOrder first, as the 'All' group.
  {
    Instances::Group g( cfg.dictionaryOrder, dictionaries, Config::Group() );

    // Add any missing entries to dictionary order
    Instances::complementDictionaryOrder( g,
                                          Instances::Group( cfg.inactiveDictionaries, dictionaries, Config::Group() ),
                                          dictionaries );

    g.name = tr( "All" );
    g.id = Instances::Group::AllGroupId;
    g.icon = "folder.png";

    groupInstances.push_back( g );
  }

  for( int x  = 0; x < cfg.groups.size(); ++x )
    groupInstances.push_back( Instances::Group( cfg.groups[ x ], dictionaries, cfg.inactiveDictionaries ) );

  // Update names for dictionaries that are present, so that they could be
  // found in case they got moved.
  Instances::updateNames( cfg, dictionaries );

  groupList->fill( groupInstances );
  groupList->setCurrentGroup( cfg.lastMainGroupId );
  updateCurrentGroupProperty();

  updateDictionaryBar();

#ifdef QT_DEBUG
  qDebug() << "Reloading all the tabs...";
#endif

  for( int i = 0; i < ui.tabWidget->count(); ++i )
  {
    ArticleView & view =
      dynamic_cast< ArticleView & >( *( ui.tabWidget->widget( i ) ) );

    view.reload();
  }

  connect( groupList, SIGNAL( currentIndexChanged( QString const & ) ),
           this, SLOT( currentGroupChanged( QString const & ) ) );
}

void MainWindow::updateDictionaryBar()
{
  if ( !dictionaryBar.toggleViewAction()->isChecked() )
    return; // It's not enabled, therefore hidden -- don't waste time

  unsigned currentId = groupList -> getCurrentGroup();
  Instances::Group * grp = groupInstances.findGroup( currentId );

  dictionaryBar.setMutedDictionaries( 0 );
  if ( grp ) { // Should always be !0, but check as a safeguard
    if( currentId == Instances::Group::AllGroupId )
      dictionaryBar.setMutedDictionaries( &cfg.mutedDictionaries );
    else
    {
      Config::Group * grp = cfg.getGroup( currentId );
      dictionaryBar.setMutedDictionaries( grp ? &grp->mutedDictionaries : 0 );
    }

    dictionaryBar.setDictionaries( grp->dictionaries );

    if ( useSmallIconsInToolbarsAction.isChecked() ) {
      int extent = QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize);
      dictionaryBar.setDictionaryIconSize( extent );
    }
  }
}

void MainWindow::makeScanPopup()
{
  scanPopup.reset();

  if ( !cfg.preferences.enableScanPopup &&
       !cfg.preferences.enableClipboardHotkey )
    return;

  scanPopup = new ScanPopup( 0, cfg, articleNetMgr,
#ifndef USE_QTWEBKIT
                             *webEngineProfile,
#endif
                             audioPlayerFactory.player(),
                             dictionaries, groupInstances, history );

  scanPopup->setStyleSheet( styleSheet() );

  if ( cfg.preferences.enableScanPopup && enableScanPopup->isChecked() )
    scanPopup->enableScanning();

  connect( scanPopup.get(), SIGNAL(editGroupRequested( unsigned ) ),
           this, SLOT(editDictionaries( unsigned )), Qt::QueuedConnection );

  connect( scanPopup.get(), SIGNAL(sendPhraseToMainWindow( Config::InputPhrase const & ) ),
           this, SLOT(phraseReceived( Config::InputPhrase const & )), Qt::QueuedConnection );

  connect( this, SIGNAL( setExpandOptionalParts( bool ) ),
           scanPopup.get(), SIGNAL( setViewExpandMode( bool ) ) );

  connect( scanPopup.get(), SIGNAL( setExpandMode( bool ) ),
           this, SLOT( setExpandMode( bool ) ) );

  connect( scanPopup.get(), SIGNAL( forceAddWordToHistory( const QString & ) ),
           this, SLOT( forceAddWordToHistory( const QString & ) ) );

  connect( scanPopup.get(), SIGNAL( showDictionaryInfo( const QString & ) ),
           this, SLOT( showDictionaryInfo( const QString & ) ) );

  connect( scanPopup.get(), SIGNAL( openDictionaryFolder( const QString & ) ),
           this, SLOT( openDictionaryFolder( const QString & ) ) );

  connect( scanPopup.get(), SIGNAL( sendWordToHistory( QString ) ),
           this, SLOT( addWordToHistory( QString ) ) );

  connect( this, SIGNAL( setPopupGroupByName( QString ) ),
           scanPopup.get(), SLOT( setGroupByName( QString ) ) );

  connect( scanPopup.get(), SIGNAL( sendWordToFavorites( QString, uint ) ),
           this, SLOT( addWordToFavorites( QString, uint ) ) );

  connect( scanPopup.get(), SIGNAL( isWordPresentedInFavorites( QString, uint ) ),
           this, SLOT( isWordPresentedInFavorites( QString, uint ) ) );

#ifdef Q_OS_WIN32
  connect( scanPopup.get(), SIGNAL( isGoldenDictWindow( HWND ) ),
           this, SLOT( isGoldenDictWindow( HWND ) ) );
#endif
}

vector< sptr< Dictionary::Class > > const & MainWindow::getActiveDicts()
{
  if ( groupInstances.empty() )
    return dictionaries;

  int current = groupList->currentIndex();

  if ( current < 0 || current >= (int) groupInstances.size() )
  {
    // This shouldn't ever happen
    return dictionaries;
  }

  Config::MutedDictionaries const * mutedDictionaries = dictionaryBar.getMutedDictionaries();
  if ( !dictionaryBar.toggleViewAction()->isChecked() || mutedDictionaries == 0 )
    return groupInstances[ current ].dictionaries;
  else
  {
    vector< sptr< Dictionary::Class > > const & activeDicts =
      groupInstances[ current ].dictionaries;

    // Populate the special dictionariesUnmuted array with only unmuted
    // dictionaries

    dictionariesUnmuted.clear();
    dictionariesUnmuted.reserve( activeDicts.size() );

    for( unsigned x = 0; x < activeDicts.size(); ++x )
      if ( !mutedDictionaries->contains(
              QString::fromStdString( activeDicts[ x ]->getId() ) ) )
        dictionariesUnmuted.push_back( activeDicts[ x ] );

    return dictionariesUnmuted;
  }
}

void MainWindow::createTabList()
{
  tabListMenu->setIcon(QIcon(":/icons/windows-list.png"));
  connect(tabListMenu, SIGNAL(aboutToShow()), this, SLOT(fillWindowsMenu()));
  connect(tabListMenu, SIGNAL(triggered(QAction*)), this, SLOT(switchToWindow(QAction*)));

  tabListButton = new QToolButton(ui.tabWidget);
  tabListButton->setAutoRaise(true);
  tabListButton->setIcon(tabListMenu->icon());
  tabListButton->setMenu(tabListMenu);
  tabListButton->setToolTip( tr( "Open Tabs List" ) );
  tabListButton->setPopupMode(QToolButton::InstantPopup);
  ui.tabWidget->setCornerWidget(tabListButton);
  tabListButton->setFocusPolicy(Qt::NoFocus);
}

void MainWindow::fillWindowsMenu()
{
  tabListMenu->clear();

  if(cfg.preferences.mruTabOrder)
  {
    for (int i = 0; i < mruList.count(); i++)
    {
      QAction *act = tabListMenu->addAction(ui.tabWidget->tabIcon(ui.tabWidget->indexOf(mruList.at(i))), ui.tabWidget->tabText(ui.tabWidget->indexOf(mruList.at(i))));

      //remember the index of the Tab to be later used in ctrlReleased()
      act->setData(ui.tabWidget->indexOf(mruList.at(i)));

      if (ui.tabWidget->currentIndex() == ui.tabWidget->indexOf(mruList.at(i)))
      {
        QFont f( act->font() );
        f.setBold( true );
        act->setFont( f );
      }
    }
    if (tabListMenu->actions().size() > 1)
    {
      tabListMenu->setActiveAction(tabListMenu->actions().at(1));
    }
  }
  else
  {
    for (int i = 0; i < ui.tabWidget->count(); i++)
    {
      QAction *act = tabListMenu->addAction( ui.tabWidget->tabIcon( i ),
      ui.tabWidget->tabText( i ) );
      act->setData( i );
      if (ui.tabWidget->currentIndex() == i)
      {
        QFont f( act->font() );
        f.setBold( true );
        act->setFont( f );
      }
    }
  }
  return;
}

void MainWindow::switchToWindow(QAction *act)
{
  int idx = act->data().toInt();
  ui.tabWidget->setCurrentIndex(idx);
}


void MainWindow::addNewTab()
{
  createNewTab( true, tr( "(untitled)" ) );
}

ArticleView * MainWindow::createNewTab( bool switchToIt,
                                        QString const & name )
{
  ArticleView * view = new ArticleView( this, articleNetMgr,
#ifndef USE_QTWEBKIT
                                        *webEngineProfile,
#endif
                                        audioPlayerFactory.player(),
                                        dictionaries, groupInstances, false, cfg,
                                        *ui.searchInPageAction,
                                        dictionaryBar.toggleViewAction(),
                                        groupList );

  connect( view, SIGNAL( titleChanged(  ArticleView *, QString const & ) ),
           this, SLOT( titleChanged(  ArticleView *, QString const & ) ) );

  connect( view, SIGNAL( iconChanged( ArticleView *, QIcon const & ) ),
           this, SLOT( iconChanged( ArticleView *, QIcon const & ) ) );

  connect( view, SIGNAL( pageLoadingStateChanged( ArticleView *, bool ) ),
           this, SLOT( pageLoadingStateChanged( ArticleView *, bool ) ) );

  connect( view, SIGNAL( canGoBackForwardChanged( ArticleView * ) ),
           this, SLOT( canGoBackForwardChanged( ArticleView * ) ) );

  connect( view, SIGNAL( pageUnloaded( ArticleView * ) ),
           this, SLOT( pageUnloaded( ArticleView * ) ) );

  connect( view, SIGNAL( articleLoaded( ArticleView *, QString const &, bool ) ),
           this, SLOT( articleLoaded( ArticleView *, QString const &, bool ) ) );

  connect( view, SIGNAL( pageLoaded( ArticleView * ) ),
           this, SLOT( pageLoaded( ArticleView * ) ) );

  connect( view, SIGNAL( openLinkInNewTab( QUrl const &, QUrl const &, QString const &, ArticleView::Contexts const & ) ),
           this, SLOT( openLinkInNewTab( QUrl const &, QUrl const &, QString const &, ArticleView::Contexts const & ) ) );

  connect( view, SIGNAL( showDefinitionInNewTab( Config::InputPhrase const &, unsigned, QString const &, ArticleView::Contexts const & ) ),
           this, SLOT( showDefinitionInNewTab( Config::InputPhrase const &, unsigned, QString const &, ArticleView::Contexts const & ) ) );

  connect( view, SIGNAL( typingEvent( QString const & ) ),
           this, SLOT( typingEvent( QString const & ) ) );

  connect( view, SIGNAL( activeArticleChanged( ArticleView const *, const QString & ) ),
           this, SLOT( activeArticleChanged( ArticleView const *, const QString & ) ) );

  connect( view, SIGNAL( statusBarMessage( QString const &, int, QPixmap const & ) ),
           this, SLOT( showStatusBarMessage( QString const &, int, QPixmap const & ) ) );

  connect( view, SIGNAL( showDictsPane( ) ), this, SLOT( showDictsPane( ) ) );

  connect( view, SIGNAL( forceAddWordToHistory( const QString & ) ),
           this, SLOT( forceAddWordToHistory( const QString & ) ) );

  connect( this, SIGNAL( setExpandOptionalParts( bool ) ),
           view, SLOT( receiveExpandOptionalParts( bool ) ) );

  connect( view, SIGNAL( setExpandMode( bool ) ), this, SLOT( setExpandMode( bool ) ) );

  connect( view, SIGNAL( sendWordToHistory( QString ) ),
           this, SLOT( addWordToHistory( QString ) ) );

  connect( view, SIGNAL( sendWordToInputLine( QString const & ) ),
           this, SLOT( sendWordToInputLine( QString const & ) ) );

  connect( view, SIGNAL( storeResourceSavePath( QString const & ) ),
           this, SLOT( storeResourceSavePath( QString const & ) ) );

  connect( view, SIGNAL( zoomIn()), this, SLOT( zoomin() ) );

  connect( view, SIGNAL( zoomOut()), this, SLOT( zoomout() ) );

  view->setSelectionBySingleClick( cfg.preferences.selectWordBySingleClick );

  int index = cfg.preferences.newTabsOpenAfterCurrentOne ?
              ui.tabWidget->currentIndex() + 1 : ui.tabWidget->count();

  QString escaped = name;
  escaped.replace( "&", "&&" );

  ui.tabWidget->insertTab( index, view, escaped );
  mruList.append(dynamic_cast<QWidget*>(view));

  if ( switchToIt )
    ui.tabWidget->setCurrentIndex( index );

  view->setZoomFactor( cfg.preferences.zoomFactor );

#ifdef Q_OS_WIN32
  view->installEventFilter( this );
#endif
  return view;
}

void MainWindow::tabCloseRequested( int x )
{
  QWidget * w = ui.tabWidget->widget( x );

  mruList.removeOne(w);

  // In MRU case: First, we switch to the appropriate tab
  // and only then remove the old one.

  //activate a tab in accordance with MRU
  if ( cfg.preferences.mruTabOrder && mruList.size() > 0 ) {
    ui.tabWidget->setCurrentWidget(mruList.at(0));
  }
  else if( ui.tabWidget->count() > 1 )
  {
    //activate neighboring tab
    int n = x >= ui.tabWidget->count() - 1 ? x - 1 : x + 1;
    if( n >= 0 )
      ui.tabWidget->setCurrentIndex( n );
  }

  ui.tabWidget->removeTab( x );
  delete w;

  if( ui.tabWidget->count() != 0 )
    return;
  // if everything is closed, add a new tab
  addNewTab();

#if QT_VERSION >= QT_VERSION_CHECK( 5, 0, 0 )
  QWidget const * const focused = focusWidget();
  if( !focused || focused == ui.tabWidget->tabBar() )
  {
    // The article view in the last closed tab had focus. Now no widget has focus or the tab bar acquired useless to it
    // focus. In this situation GoldenDict ignores typing a new phrase to be translated. Furthermore, when no widget has
    // focus, focus-transferring shortcuts don't work. Give focus to the newly created article view to work this around.
    // Cannot just call focusArticleView() or focusTranslateLine(), because no window should be activated here.
    Q_ASSERT( getCurrentArticleView() );
    getCurrentArticleView()->focus();
  }
#endif
}

void MainWindow::closeCurrentTab()
{
  tabCloseRequested( ui.tabWidget->currentIndex() );
}

void MainWindow::closeAllTabs()
{
  while (ui.tabWidget->count() > 1)
    closeCurrentTab();

  // close last tab
  closeCurrentTab();
}

void MainWindow::closeRestTabs()
{
  if ( ui.tabWidget->count() < 2 )
    return;

  int idx = ui.tabWidget->currentIndex();

  for (int i = 0; i < idx; i++)
    tabCloseRequested(0);

  ui.tabWidget->setCurrentIndex(0);

  while (ui.tabWidget->count() > 1)
    tabCloseRequested(1);
}

void MainWindow::switchToNextTab()
{
  if ( ui.tabWidget->count() < 2 )
    return;

  ui.tabWidget->setCurrentIndex( ( ui.tabWidget->currentIndex() + 1 ) % ui.tabWidget->count() );
}

void MainWindow::switchToPrevTab()
{
  if ( ui.tabWidget->count() < 2 )
    return;

  if ( !ui.tabWidget->currentIndex() )
    ui.tabWidget->setCurrentIndex( ui.tabWidget->count() - 1 );
  else
    ui.tabWidget->setCurrentIndex( ui.tabWidget->currentIndex() - 1 );
}

//emitted by tabListMenu when user releases Ctrl
void MainWindow::ctrlReleased()
{
    if (tabListMenu->actions().size() > 1)
    {
        QAction *act = tabListMenu->activeAction();
        if( act == 0 )
          act = tabListMenu->actions().at( 1 );
        ui.tabWidget->setCurrentIndex( act->data().toInt() );
    }
    tabListMenu->hide();
}

void MainWindow::backClicked()
{
  GD_DPRINTF( "Back\n" );

  ArticleView *view = getCurrentArticleView();

  view->back();
}

void MainWindow::forwardClicked()
{
  GD_DPRINTF( "Forward\n" );

  ArticleView *view = getCurrentArticleView();

  view->forward();
}

void MainWindow::titleChanged( ArticleView * view, QString const & title )
{
  QString escaped = title;
  escaped.replace( "&", "&&" );
  Folding::prepareToEmbedRTL( escaped );

  int index = ui.tabWidget->indexOf( view );
  ui.tabWidget->setTabText( index, escaped );

  if( index == ui.tabWidget->currentIndex() )
  {
    // Set icon for "Add to Favorites" action
    if( isWordPresentedInFavorites( title, cfg.lastMainGroupId ) )
    {
      addToFavorites->setIcon( blueStarIcon );
      addToFavorites->setToolTip( tr( "Remove current tab from Favorites" ) );
    }
    else
    {
      addToFavorites->setIcon( starIcon );
      addToFavorites->setToolTip( tr( "Add current tab to Favorites" ) );
    }

    updateWindowTitle();
  }
}

void MainWindow::iconChanged( ArticleView * view, QIcon const & icon )
{
  ui.tabWidget->setTabIcon( ui.tabWidget->indexOf( view ), groupInstances.size() > 1 ? icon : QIcon() );
}

void MainWindow::updateWindowTitle()
{
  ArticleView *view = getCurrentArticleView();
  if ( view )
  {
    QString str = view->getTitle();
    if( !str.isEmpty() )
    {
      if( blockUpdateWindowTitle )
      {
        blockUpdateWindowTitle = false;
        return;
      }

      Folding::prepareToEmbedRTL( str );
      setWindowTitle( tr( "%1 - %2" ).arg( str, "GoldenDict" ) );
    }
  }
}

void MainWindow::pageLoadingStateChanged( ArticleView * view, bool isLoading )
{
  if( view == getCurrentArticleView() )
    setIsPageLoading( isLoading );
  // else: ignore this change in a non-active tab
}

void MainWindow::canGoBackForwardChanged( ArticleView * view )
{
  if( view == getCurrentArticleView() )
    updateBackForwardButtons( view );
  // else: ignore this change in a non-active tab
}

void MainWindow::pageUnloaded( ArticleView * view )
{
  if( view != getCurrentArticleView() )
    return; // It was background action

  navPronounce->setEnabled( false );
  if( ui.dictsList->isVisible() )
    ui.dictsList->clear();
}

void MainWindow::articleLoaded( ArticleView * view, QString const & id, bool isActive )
{
  if( view != getCurrentArticleView() )
    return; // It was background action

  navPronounce->setEnabled( view->hasSound() );
  if( ui.dictsList->isVisible() )
    appendToFoundInDictsList( id, isActive );
}

void MainWindow::pageLoaded( ArticleView * view )
{
  if( view != getCurrentArticleView() )
    return; // It was background action

  if ( cfg.preferences.pronounceOnLoadMain )
    pronounce( view );
}

void MainWindow::showStatusBarMessage( QString const & message, int timeout, QPixmap const & icon )
{
  if( message.isEmpty() )
    mainStatusBar->clearMessage();
  else
    mainStatusBar->showMessage( message, timeout, icon );
}

void MainWindow::tabSwitched( int )
{
  translateBox->setPopupEnabled( false );
  updateIsPageLoading();
  updateBackForwardButtons();
  updatePronounceAvailability();
  updateFoundInDictsList();
  updateWindowTitle();
  if (mruList.size() > 1)
  {
    int from = mruList.indexOf( ui.tabWidget->widget( ui.tabWidget->currentIndex() ) );
    if ( from > 0)
      mruList.move( from, 0 );
  }

  // Set icon for "Add to Favorites" action
  QString headword = ui.tabWidget->tabText( ui.tabWidget->currentIndex() );
  if( isWordPresentedInFavorites( unescapeTabHeader( headword ), cfg.lastMainGroupId ) )
  {
    addToFavorites->setIcon( blueStarIcon );
    addToFavorites->setToolTip( tr( "Remove current tab from Favorites" ) );
  }
  else
  {
    addToFavorites->setIcon( starIcon );
    addToFavorites->setToolTip( tr( "Add current tab to Favorites" ) );
  }
}

void MainWindow::tabMenuRequested(QPoint pos)
{
//  // do not show this menu for single tab
//  if ( ui.tabWidget->count() < 2 )
//    return;

  tabMenu->popup(ui.tabWidget->mapToGlobal(pos));
}

void MainWindow::dictionaryBarToggled( bool )
{
  // From now on, only the triggered() signal is interesting to us
  disconnect( dictionaryBar.toggleViewAction(), SIGNAL(toggled(bool)),
              this, SLOT(dictionaryBarToggled(bool)) );

  updateDictionaryBar(); // Updates dictionary bar contents if it's shown
  applyMutedDictionariesState(); // Visibility change affects searches and results
}

void MainWindow::pronounce( ArticleView * view )
{
  if ( view )
    view->playSound();
  else
    getCurrentArticleView()->playSound();
}

void MainWindow::showDictsPane( )
{
  if( !ui.dictsPane->isVisible() )
    ui.dictsPane->show();
}

void MainWindow::dictsPaneVisibilityChanged( bool visible )
{
  if (visible) {
    updateFoundInDictsList();
  }
}

void MainWindow::appendToFoundInDictsList( QString const & id, bool isActive )
{
  QByteArray const idUtf8 = id.toUtf8();
  for( unsigned x = dictionaries.size(); x--; )
  {
    sptr< Dictionary::Class > & dictionary = dictionaries[ x ];
    if( dictionary->getId() != idUtf8.constData() )
      continue;

    QString const dictName = QString::fromUtf8( dictionary->getName().c_str() );
    QListWidgetItem * const item =
        new QListWidgetItem(
          dictionary->getIcon().pixmap( 32 ).scaledToHeight( 21, Qt::SmoothTransformation ),
          dictName, ui.dictsList, QListWidgetItem::Type );
    item->setData( Qt::UserRole, id );
    item->setToolTip( dictName );

    ui.dictsList->addItem( item );
    if( isActive )
      ui.dictsList->setCurrentItem( item );
    break;
  }
}

void MainWindow::updateFoundInDictsList()
{
  if (!ui.dictsList->isVisible())
  {
    // nothing to do, the list is not visible
    return;
  }

  ui.dictsList->clear();

  ArticleView *view = getCurrentArticleView();

  if ( view )
  {
    QStringList const ids = view->getArticleList();
    QString activeId = view->getActiveArticleId();

    for( QStringList::const_iterator i = ids.constBegin(); i != ids.constEnd(); ++i)
      appendToFoundInDictsList( *i, *i == activeId );
  }
}

void MainWindow::updateIsPageLoading()
{
  if( ArticleView const * view = getCurrentArticleView() )
    setIsPageLoading( view->isPageLoading() );
}

void MainWindow::setIsPageLoading( bool isLoading )
{
  if( cfg.preferences.searchInDock )
    ui.loadingIndicatorLabel->setVisible( isLoading );
  else
    translateBox->setIsPageLoading( isLoading );
}

void MainWindow::updateBackForwardButtons()
{
  ArticleView *view = getCurrentArticleView();

  if ( view )
    updateBackForwardButtons( view );
}

void MainWindow::updateBackForwardButtons( ArticleView * view )
{
  Q_ASSERT( view );
  Q_ASSERT( view == getCurrentArticleView() );

  navBack->setEnabled( view->canGoBack() );
  navForward->setEnabled( view->canGoForward() );
}

void MainWindow::updatePronounceAvailability()
{
  bool pronounceEnabled = ui.tabWidget->count() > 0 &&
    getCurrentArticleView()->hasSound();

  navPronounce->setEnabled( pronounceEnabled );
}

void MainWindow::editDictionaries( unsigned editDictionaryGroup )
{
  hotkeyWrapper.reset(); // No hotkeys while we're editing dictionaries
  scanPopup.reset(); // No scan popup either. No one should use dictionaries.
  closeHeadwordsDialog();
  closeFullTextSearchDialog();

  ftsIndexing.stopIndexing();
  ftsIndexing.clearDictionaries();

  wordFinder.clear();
  dictionariesUnmuted.clear();

  hideGDHelp();

  { // Limit existence of newCfg

  Config::Class newCfg = cfg;
  EditDictionaries dicts( this, newCfg, dictionaries, groupInstances, dictNetMgr );

  connect( &dicts, SIGNAL( showDictionaryInfo( QString const & ) ),
           this, SLOT( showDictionaryInfo( QString const & ) ) );

  connect( &dicts, SIGNAL( showDictionaryHeadwords( QString const & ) ),
           this, SLOT( showDictionaryHeadwords( QString const & ) ) );

  if ( editDictionaryGroup != Instances::Group::NoGroupId )
    dicts.editGroup( editDictionaryGroup );

  dicts.restoreGeometry( cfg.dictionariesDialogGeometry );
  dicts.exec();
  cfg.dictionariesDialogGeometry = newCfg.dictionariesDialogGeometry = dicts.saveGeometry();

  if ( dicts.areDictionariesChanged() || dicts.areGroupsChanged() )
  {

    // Set muted dictionaries from old groups
    for( int x = 0; x < newCfg.groups.size(); x++ )
    {
      unsigned id = newCfg.groups[ x ].id;
      if( id != Instances::Group::NoGroupId )
      {
        Config::Group const * grp = cfg.getGroup( id );
        if( grp )
        {
          newCfg.groups[ x ].mutedDictionaries = grp->mutedDictionaries;
          newCfg.groups[ x ].popupMutedDictionaries = grp->popupMutedDictionaries;
        }
      }
    }

    cfg = newCfg;

    updateGroupList();

    Config::save( cfg );

    updateSuggestionList();
  }

  }

  makeScanPopup();
  installHotKeys();

  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    dictionaries[ x ]->setFTSParameters( cfg.preferences.fts );
    dictionaries[ x ]->setSynonymSearchEnabled( cfg.preferences.synonymSearchEnabled );
  }

  ftsIndexing.setDictionaries( dictionaries );
  ftsIndexing.doIndexing();
}

void MainWindow::editCurrentGroup()
{
  editDictionaries( groupList->getCurrentGroup() );
}

void MainWindow::editPreferences()
{
  hotkeyWrapper.reset(); // So we could use the keys it hooks
  scanPopup.reset(); // No scan popup either. No one should use dictionaries.
  closeHeadwordsDialog();
  closeFullTextSearchDialog();

  ftsIndexing.stopIndexing();
  ftsIndexing.clearDictionaries();

  Preferences preferences( this, cfg );

  hideGDHelp();

  preferences.show();

  if ( preferences.exec() == QDialog::Accepted )
  {
    Config::Preferences p = preferences.getPreferences();

    // These parameters are not set in dialog
    p.zoomFactor = cfg.preferences.zoomFactor;
    p.helpZoomFactor = cfg.preferences.helpZoomFactor;
    p.wordsZoomLevel = cfg.preferences.wordsZoomLevel;
    p.hideMenubar = cfg.preferences.hideMenubar;
    p.searchInDock = cfg.preferences.searchInDock;
    p.alwaysOnTop = cfg.preferences.alwaysOnTop;
#ifndef Q_WS_X11
    p.trackClipboardChanges = cfg.preferences.trackClipboardChanges;
#endif
    p.proxyServer.systemProxyUser = cfg.preferences.proxyServer.systemProxyUser;
    p.proxyServer.systemProxyPassword = cfg.preferences.proxyServer.systemProxyPassword;

    p.fts.dialogGeometry = cfg.preferences.fts.dialogGeometry;
    p.fts.matchCase = cfg.preferences.fts.matchCase;
    p.fts.maxArticlesPerDictionary = cfg.preferences.fts.maxArticlesPerDictionary;
    p.fts.maxDistanceBetweenWords = cfg.preferences.fts.maxDistanceBetweenWords;
    p.fts.searchMode = cfg.preferences.fts.searchMode;
    p.fts.useMaxArticlesPerDictionary = cfg.preferences.fts.useMaxArticlesPerDictionary;
    p.fts.useMaxDistanceBetweenWords = cfg.preferences.fts.useMaxDistanceBetweenWords;
    p.fts.ignoreWordsOrder = cfg.preferences.fts.ignoreWordsOrder;
    p.fts.ignoreDiacritics = cfg.preferences.fts.ignoreDiacritics;

    bool needReload = false;

    // See if we need to reapply stylesheets
    if ( cfg.preferences.displayStyle != p.displayStyle || cfg.preferences.addonStyle != p.addonStyle )
    {
      applyQtStyleSheet( p.displayStyle, p.addonStyle );
      articleMaker.setDisplayStyle( p.displayStyle, p.addonStyle );
      needReload = true;
    }

    if( cfg.preferences.collapseBigArticles != p.collapseBigArticles
        || cfg.preferences.articleSizeLimit != p.articleSizeLimit )
    {
      articleMaker.setCollapseParameters( p.collapseBigArticles, p.articleSizeLimit );
    }

    // See if we need to reapply expand optional parts mode
    if( cfg.preferences.alwaysExpandOptionalParts != p.alwaysExpandOptionalParts )
    {
      emit setExpandOptionalParts( p.alwaysExpandOptionalParts );
      // Signal setExpandOptionalParts reload all articles
      needReload = false;
    }

    // See if we need to change help language
    if( cfg.preferences.helpLanguage != p.helpLanguage )
      closeGDHelp();

#ifndef USE_QTWEBKIT
    ArticleView::updateProfilePreferences( *webEngineProfile, cfg.preferences, p );
#endif

    for( int x = 0; x < ui.tabWidget->count(); ++x )
    {
      ArticleView & view =
        dynamic_cast< ArticleView & >( *( ui.tabWidget->widget( x ) ) );

      view.setSelectionBySingleClick( p.selectWordBySingleClick );

      if( needReload )
        view.reload();
    }

    if( cfg.preferences.historyStoreInterval != p.historyStoreInterval )
      history.setSaveInterval( p.historyStoreInterval );

    if( cfg.preferences.favoritesStoreInterval != p.favoritesStoreInterval )
      ui.favoritesPaneWidget->setSaveInterval( p.favoritesStoreInterval );

    if( cfg.preferences.maxNetworkCacheSize != p.maxNetworkCacheSize )
      setupNetworkCache( p.maxNetworkCacheSize );
    cfg.preferences = p;

    audioPlayerFactory.setPreferences( cfg.preferences );

    beforeScanPopupSeparator->setVisible( cfg.preferences.enableScanPopup );
    enableScanPopup->setVisible( cfg.preferences.enableScanPopup );
    afterScanPopupSeparator->setVisible( cfg.preferences.enableScanPopup );

    if ( !cfg.preferences.enableScanPopup )
      enableScanPopup->setChecked( false );

    updateTrayIcon();
    applyProxySettings();
    applyWebSettings();

    ui.tabWidget->setHideSingleTab(cfg.preferences.hideSingleTab);

    setAutostart( cfg.preferences.autoStart );

    prepareNewReleaseChecks();

    history.enableAdd( cfg.preferences.storeHistory );
    history.setMaxSize( cfg.preferences.maxStringsInHistory );
    ui.historyPaneWidget->updateHistoryCounts();

    for( unsigned x = 0; x < dictionaries.size(); x++ )
    {
      dictionaries[ x ]->setFTSParameters( cfg.preferences.fts );
      dictionaries[ x ]->setSynonymSearchEnabled( cfg.preferences.synonymSearchEnabled );
    }

    ui.fullTextSearchAction->setEnabled( cfg.preferences.fts.enabled );

    Config::save( cfg );
  }

  makeScanPopup();
  installHotKeys();

  ftsIndexing.setDictionaries( dictionaries );
  ftsIndexing.doIndexing();
}

void MainWindow::currentGroupChanged( QString const & )
{
  cfg.lastMainGroupId = groupList->getCurrentGroup();
  Instances::Group const * igrp = groupInstances.findGroup( cfg.lastMainGroupId );
  if( cfg.lastMainGroupId == Instances::Group::AllGroupId )
  {
    if( igrp )
      igrp->checkMutedDictionaries( &cfg.mutedDictionaries );
    dictionaryBar.setMutedDictionaries( &cfg.mutedDictionaries );
  }
  else
  {
    Config::Group * grp = cfg.getGroup( cfg.lastMainGroupId );
    if( grp )
    {
      if( igrp )
        igrp->checkMutedDictionaries( &grp->mutedDictionaries );
      dictionaryBar.setMutedDictionaries( &grp->mutedDictionaries );
    }
    else
      dictionaryBar.setMutedDictionaries( 0 );
  }

  updateDictionaryBar();

  // Update word search results
  translateBox->setPopupEnabled( false );
  updateSuggestionList();
  translateInputFinished( false );

  updateCurrentGroupProperty();

  if( ftsDlg )
    ftsDlg->setCurrentGroup( cfg.lastMainGroupId );
}

void MainWindow::updateCurrentGroupProperty()
{
  // We maintain currentGroup property so styles could use that to change
  // fonts based on group names
  Instances::Group * grp =
      groupInstances.findGroup( groupList->getCurrentGroup() );

  if ( grp && translateLine->property( "currentGroup" ).toString() !=
       grp->name )
  {
    translateLine->setProperty( "currentGroup", grp->name );
    wordList->setProperty( "currentGroup", grp->name );
    QString ss = styleSheet();

    // Only update stylesheet if it mentions currentGroup, as updating the
    // stylesheet is a slow operation
    if ( ss.contains("currentGroup") )
      setStyleSheet( ss );
  }
}

void MainWindow::translateInputChanged( QString const & newValue )
{
  updateSuggestionList( newValue );
  translateBoxSuffix = QString();
}

void MainWindow::updateSuggestionList()
{
  updateSuggestionList( translateLine->text() );
}

void MainWindow::updateSuggestionList( QString const & newValue )
{
  // If there's some status bar message present, clear it since it may be
  // about the previous search that has failed.
  if ( !mainStatusBar->currentMessage().isEmpty() )
  {
    mainStatusBar->clearMessage();
  }

  // If some word is selected in the word list, unselect it. This prevents
  // triggering a set of spurious activation signals when the list changes.

  if ( wordList->selectionModel()->hasSelection() )
    wordList->setCurrentItem( 0, QItemSelectionModel::Clear );

  QString req = newValue.trimmed();

  if ( !req.size() )
  {
    // An empty request always results in an empty result
    wordFinder.cancel();
    wordList->clear();
    wordList->unsetCursor();

    // Reset the noResults mark if it's on right now
    if ( translateLine->property( "noResults" ).toBool() )
    {
      translateLine->setProperty( "noResults", false );
      setStyleSheet( styleSheet() );
    }
    return;
  }

  wordList->setCursor( Qt::WaitCursor );

  wordFinder.prefixMatch( req, getActiveDicts() );
}

void MainWindow::translateInputFinished( bool checkModifiers )
{
  QString word = translateLine->text().trimmed();
  if( word.isEmpty() )
    return;
  word = Folding::unescapeWildcardSymbols( word );
  respondToTranslationRequest( Config::InputPhrase( word, translateBoxSuffix ), checkModifiers );
}

void MainWindow::respondToTranslationRequest( Config::InputPhrase const & phrase,
                                              bool checkModifiers, QString const & scrollTo )
{
  if ( phrase.isValid() )
  {
    Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
    if ( checkModifiers && ( mods & (Qt::ControlModifier | Qt::ShiftModifier) ) )
      addNewTab();

    showTranslationFor( phrase, 0, scrollTo );

    if ( cfg.preferences.searchInDock )
    {
      if ( ui.searchPane->isFloating() )
        activateWindow();
    }

    getCurrentArticleView()->focus();
  }
}

void MainWindow::setTranslateBoxTextAndKeepSuffix( QString text, WildcardPolicy wildcardPolicy,
                                                   TranslateBoxPopup popupAction )
{
  if( wildcardPolicy == EscapeWildcards )
    text = Folding::escapeWildcardSymbols( text );

  if( popupAction == NoPopupChange || cfg.preferences.searchInDock )
    translateLine->setText( text );
  else
    translateBox->setText( text, popupAction == EnablePopup );
}

void MainWindow::setTranslateBoxTextAndClearSuffix( QString const & text, WildcardPolicy wildcardPolicy,
                                                    TranslateBoxPopup popupAction )
{
  setTranslateBoxTextAndKeepSuffix( text, wildcardPolicy, popupAction );
  translateBoxSuffix = QString();
}

void MainWindow::handleEsc()
{
  ArticleView *view = getCurrentArticleView();
  if ( view && view->closeSearch() )
    return;

  if( cfg.preferences.escKeyHidesMainWindow )
  {
    toggleMainWindow();
  }
  else
    focusTranslateLine();
}

void MainWindow::focusTranslateLine()
{
  if ( cfg.preferences.searchInDock )
  {
    if ( ui.searchPane->isFloating() )
      ui.searchPane->activateWindow();
  }
  else
  {
    if ( !isActiveWindow() )
      activateWindow();
  }

  translateLine->clearFocus();
  translateLine->setFocus();
  translateLine->selectAll();
}

void MainWindow::applyMutedDictionariesState()
{
  translateBox->setPopupEnabled( false );

  updateSuggestionList();

  ArticleView *view = getCurrentArticleView();

  if ( view )
  {
    // Update active article view
    view->updateMutedContents();
  }
}

bool MainWindow::handleBackForwardMouseButtons ( QMouseEvent * event) {
  if ( event->button() == Qt::XButton1 ) {
    backClicked();
    return true;
  }
  else
  if ( event->button() == Qt::XButton2 ) {
    forwardClicked();
    return true;
  }
  else
    return false;
}

bool MainWindow::eventFilter( QObject * obj, QEvent * ev )
{
#ifdef Q_OS_WIN
  if( obj == this && ev->type() == gdStoreNormalGeometryEvent )
  {
    if( !isMaximized() && !isMinimized() && !isFullScreen() )
      cfg.normalMainWindowGeometry = normalGeometry();
    ev->accept();
    return true;
  }

  if( obj == this && ev->type() == gdApplyNormalGeometryEvent )
  {
    if( !isMaximized() && !isMinimized() && !isFullScreen() )
      setGeometry( cfg.normalMainWindowGeometry );
    ev->accept();
    return true;
  }
#endif
  if ( ev->type() == QEvent::ShortcutOverride
       || ev->type() == QEvent::KeyPress )
  {
    // Handle Ctrl+H to show the History Pane.
    QKeyEvent * ke = static_cast<QKeyEvent*>( ev );
    if ( ke->key() == Qt::Key_H && ke->modifiers() == Qt::ControlModifier )
    {
      if( ev->type() == QEvent::KeyPress )
        on_showHideHistory_triggered();
      ev->accept();
      return true;
    }

    // Handle Ctrl+I to show the Favorities Pane.
    if ( ke->key() == Qt::Key_I && ke->modifiers() == Qt::ControlModifier )
    {
      if( ev->type() == QEvent::KeyPress )
        on_showHideFavorites_triggered();
      ev->accept();
      return true;
    }

    // Handle F3/Shift+F3 shortcuts
    if ( ke->key() == Qt::Key_F3 )
    {
      ArticleView  * view = getCurrentArticleView();
      if ( view  && view->handleF3( obj, ev ) )
        return true;
    }
  }

  // when the main window is moved or resized, hide the word list suggestions
  if ( obj == this && ( ev->type() == QEvent::Move || ev->type() == QEvent::Resize ) )
  {
#ifdef Q_OS_WIN
    if( !isMaximized() && !isMinimized() && !isFullScreen() && gdAskMessage != 0xFFFFFFFF )
    {
      QEvent *ev = new QEvent( gdStoreNormalGeometryEvent );
      qApp->postEvent( this, ev );
    }
#endif
    if ( !cfg.preferences.searchInDock )
    {
        translateBox->setPopupEnabled( false );
        return false;
    }
  }

  if ( obj == this && ev->type() == QEvent::WindowStateChange )
  {
    QWindowStateChangeEvent *stev = static_cast< QWindowStateChangeEvent *>( ev );
    wasMaximized = ( stev->oldState() == Qt::WindowMaximized && isMinimized() );

#ifdef Q_OS_WIN
    if( stev->oldState() == Qt::WindowMaximized && !isMinimized() && cfg.normalMainWindowGeometry.width() > 0 )
    {
      QEvent *ev = new QEvent( gdApplyNormalGeometryEvent );
      qApp->postEvent( this, ev );
    }
#endif
  }

  if ( ev->type() == QEvent::MouseButtonPress ) {
    QMouseEvent * event = static_cast< QMouseEvent * >( ev );

    // clicks outside of the word list should hide it.
    if (obj != translateBox->wordList() && obj != translateBox->wordList()->viewport()) {
      translateBox->setPopupEnabled( false );
    }

    return handleBackForwardMouseButtons( event );
  }

  if (ev->type() == QEvent::KeyPress)
  {
    QKeyEvent *keyevent = static_cast<QKeyEvent*>(ev);

    bool handleCtrlTab = ( obj == translateLine
                           || obj == wordList
                           || obj == ui.historyList
                           || obj == ui.favoritesTree
                           || obj == ui.dictsList
                           || obj == groupList );

    if (keyevent->modifiers() == Qt::ControlModifier && keyevent->key() == Qt::Key_Tab)
    {
      if (cfg.preferences.mruTabOrder)
      {
        ctrlTabPressed();
        return true;
      }
      else if( handleCtrlTab )
      {
        QApplication::sendEvent( ui.tabWidget, ev );
        return true;
      }
      return false;
    }
    if( handleCtrlTab && keyevent->matches( QKeySequence::PreviousChild ) ) // Handle only Ctrl+Shist+Tab here because Ctrl+Tab was already handled before
    {
      QApplication::sendEvent( ui.tabWidget, ev );
      return true;
    }
  }

  if ( obj == translateLine )
  {
    if ( ev->type() == QEvent::KeyPress )
    {
      QKeyEvent * keyEvent = static_cast< QKeyEvent * >( ev );

      if ( cfg.preferences.searchInDock )
      {
        if ( keyEvent->matches( QKeySequence::MoveToNextLine ) && wordList->count() )
        {
          wordList->setFocus( Qt::ShortcutFocusReason );
          wordList->setCurrentRow( 0, QItemSelectionModel::ClearAndSelect );
          return true;
        }
      }

    }

    if ( ev->type() == QEvent::FocusIn ) {
      QFocusEvent * focusEvent = static_cast< QFocusEvent * >( ev );

      // select all on mouse click
      if ( focusEvent->reason() == Qt::MouseFocusReason ) {
        QTimer::singleShot(0, this, SLOT(focusTranslateLine()));
      }
      return false;
    }

    if ( ev->type() == QEvent::Resize ) {
      QResizeEvent * resizeEvent = static_cast< QResizeEvent * >( ev );
      groupList->setFixedHeight( resizeEvent->size().height() );
      return false;
    }
  }
  else
  if ( obj == wordList )
  {
    if ( ev->type() == QEvent::KeyPress )
    {
      QKeyEvent * keyEvent = static_cast< QKeyEvent * >( ev );

      if ( keyEvent->matches( QKeySequence::MoveToPreviousLine ) &&
           !wordList->currentRow() )
      {
        wordList->setCurrentRow( 0, QItemSelectionModel::Clear );
        translateLine->setFocus( Qt::ShortcutFocusReason );
        return true;
      }

      if ( keyEvent->matches( QKeySequence::InsertParagraphSeparator ) &&
           wordList->selectedItems().size() )
      {
        if ( cfg.preferences.searchInDock )
        {
          if ( ui.searchPane->isFloating() )
            activateWindow();
        }

        getCurrentArticleView()->focus();

        return cfg.preferences.searchInDock;
      }

      // Handle typing events used to initiate new lookups
      // TODO: refactor to eliminate duplication (see below)

      if ( keyEvent->modifiers() &
           ( Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier ) )
        return false; // A non-typing modifier is pressed

      if ( keyEvent->key() == Qt::Key_Space ||
           keyEvent->key() == Qt::Key_Backspace ||
           keyEvent->key() == Qt::Key_Tab ||
           keyEvent->key() == Qt::Key_Backtab )
        return false; // Those key have other uses than to start typing
                      // or don't make sense

      QString text = keyEvent->text();

      if ( text.size() )
      {
        typingEvent( text );
        return true;
      }
    }
  }
  else
  if (obj == ui.dictsList) {
    if ( ev->type() == QEvent::KeyPress )
    {
      QKeyEvent * keyEvent = static_cast< QKeyEvent * >( ev );

      // Handle typing events used to initiate new lookups
      // TODO: refactor to eliminate duplication (see above)

      if ( keyEvent->modifiers() &
           ( Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier ) )
        return false; // A non-typing modifier is pressed

      if ( keyEvent->key() == Qt::Key_Space ||
           keyEvent->key() == Qt::Key_Backspace ||
           keyEvent->key() == Qt::Key_Tab ||
           keyEvent->key() == Qt::Key_Backtab )
        return false; // Those key have other uses than to start typing
                      // or don't make sense

      QString text = keyEvent->text();

      if ( text.size() )
      {
        typingEvent( text );
        return true;
      }
    }
  }
    return QMainWindow::eventFilter( obj, ev );

  return false;
}

void MainWindow::wordListItemActivated( QListWidgetItem * item )
{
  if( wordListSelChanged )
    wordListSelChanged = false;
  else {
    // TODO: code duplication with translateInputFinished!

    Qt::KeyboardModifiers mods = QApplication::keyboardModifiers();
    if ( mods & (Qt::ControlModifier | Qt::ShiftModifier) )
      addNewTab();

    showTranslationFor( item->text() );
    getCurrentArticleView()->focus();
  }
}

void MainWindow::wordListSelectionChanged()
{
  QList< QListWidgetItem * > selected = wordList->selectedItems();

  if ( selected.size() )
  {
    wordListSelChanged = true;
    showTranslationFor( selected.front()->text() );
  }
}

void MainWindow::dictsListItemActivated( QListWidgetItem * item )
{
  jumpToDictionary( item, true );
}

void MainWindow::dictsListSelectionChanged()
{
  QList< QListWidgetItem * > selected = ui.dictsList->selectedItems();
  if ( selected.size() )
    jumpToDictionary( selected.front() );
}

void MainWindow::jumpToDictionary( QListWidgetItem * item, bool force )
{
  ArticleView * view = getCurrentArticleView();
  if ( view )
  {
    view->jumpToDictionary( item->data( Qt::UserRole ).toString(), force );
  }
}

void MainWindow::openLinkInNewTab( QUrl const & url,
                                   QUrl const & referrer,
                                   QString const & fromArticle,
                                   ArticleView::Contexts const & contexts )
{
  createNewTab( !cfg.preferences.newTabsOpenInBackground, "" )->
      openLink( url, referrer, fromArticle, contexts );
}

void MainWindow::showDefinitionInNewTab( Config::InputPhrase const & phrase,
                                         unsigned group,
                                         QString const & fromArticle,
                                         ArticleView::Contexts const & contexts )
{
  createNewTab( !cfg.preferences.newTabsOpenInBackground, phrase.phrase )->
      showDefinition( phrase, group, fromArticle, contexts );
}

void MainWindow::activeArticleChanged( ArticleView const * view, QString const & id )
{
  if( view != getCurrentArticleView() )
    return; // It was background action

  // select the row with the corresponding id
  for (int i = 0; i < ui.dictsList->count(); ++i) {
    QListWidgetItem * w = ui.dictsList->item( i );
    QString dictId = w->data( Qt::UserRole ).toString();

    if ( dictId == id )
    {
      // update the current row, but only if necessary
      if ( i != ui.dictsList->currentRow() )
      {
        ui.dictsList->setCurrentRow(i);
      }
      return;
    }
  }
}

void MainWindow::typingEvent( QString const & t )
{
  if ( t == "\n" || t == "\r" )
  {
    if( translateLine->isEnabled() )
      focusTranslateLine();
  }
  else
  {
    if ( ( cfg.preferences.searchInDock && ui.searchPane->isFloating() ) || ui.dictsPane->isFloating() )
      ui.searchPane->activateWindow();

    if( translateLine->isEnabled() )
    {
      translateLine->setFocus();
      // Escaping the typed-in characters is the user's responsibility.
      setTranslateBoxTextAndClearSuffix( t, WildcardsAreAlreadyEscaped, EnablePopup );
      translateLine->setCursorPosition( t.size() );
    }
  }
}

void MainWindow::mutedDictionariesChanged()
{
  if ( dictionaryBar.toggleViewAction()->isChecked() )
    applyMutedDictionariesState();
}

void MainWindow::showHistoryItem( QString const & word )
{
  // qDebug() << "Showing history item" << word;

  history.enableAdd( false );

  setTranslateBoxTextAndClearSuffix( word, EscapeWildcards, DisablePopup );
  showTranslationFor( word );

  history.enableAdd( cfg.preferences.storeHistory );
}

void MainWindow::showTranslationFor( Config::InputPhrase const & phrase,
                                     unsigned inGroup,
                                     QString const & scrollTo )
{
  ArticleView *view = getCurrentArticleView();

  navPronounce->setEnabled( false );

  unsigned group = inGroup ? inGroup :
                   ( groupInstances.empty() ? 0 :
                        groupInstances[ groupList->currentIndex() ].id );

  view->showDefinition( phrase, group, scrollTo );

  #if 0
  QUrl req;

  req.setScheme( "gdlookup" );
  req.setHost( "localhost" );
  req.addQueryItem( "word", inWord );
  req.addQueryItem( "group",
                    cfg.groups.empty() ? "" :
                      groupInstances[ groupList->currentIndex() ].name );

  ui.definition->load( req );

  return;
#endif

  #if 0
  wstring word = inWord.trimmed().toStdWString();

  // Where to look?

  vector< sptr< Dictionary::Class > > const & activeDicts = getActiveDicts();

  // Accumulate main forms

  vector< wstring > alts;

  {
    set< wstring > altsSet;

    for( unsigned x = 0; x < activeDicts.size(); ++x )
    {
      vector< wstring > found = activeDicts[ x ]->findHeadwordsForSynonym( word );

      altsSet.insert( found.begin(), found.end() );
    }

    alts.insert( alts.begin(), altsSet.begin(), altsSet.end() );
  }

  for( unsigned x = 0; x < alts.size(); ++x )
  {
    DPRINTF( "Alt: %ls\n", alts[ x ].c_str() );
  }


  string result =
    "<html><head>"
    "<meta http-equiv=\"Content-Type\" content=\"text/html; charset=utf-8\">";

  QFile cssFile( Config::getUserCssFileName() );

  if ( cssFile.open( QFile::ReadOnly ) )
  {
    result += "<style type=\"text/css\">\n";
    result += cssFile.readAll().data();
    result += "</style>\n";
  }

  result += "</head><body>";

  for( unsigned x = 0; x < activeDicts.size(); ++x )
  {
    try
    {
      string body = activeDicts[ x ]->getArticle( word, alts );

      DPRINTF( "From %s: %s\n", activeDicts[ x ]->getName().c_str(), body.c_str() );

      result += "<div class=\"gddictname\">From " + activeDicts[ x ]->getName() + "</div>" + body;
    }
    catch( Dictionary::exNoSuchWord & )
    {
      continue;
    }
  }

  result += "</body></html>";

  ArticleMaker am( dictionaries, groupInstances );

  string result = am.makeDefinitionFor( inWord, "En" );

  ui.definition->setContent( result.c_str(), QString() );

  #endif

  //ui.tabWidget->setTabText( ui.tabWidget->indexOf(ui.tab), inWord.trimmed() );
}

void MainWindow::showTranslationFor( QString const & word )
{
  showTranslationFor( Config::InputPhrase::fromPhrase( word ) );
}

void MainWindow::showTranslationFor( QString const & inWord,
                                     QStringList const & dictIDs,
                                     QRegExp const & searchRegExp,
                                     bool ignoreDiacritics )
{
  ArticleView *view = getCurrentArticleView();

  navPronounce->setEnabled( false );

  view->showDefinition( inWord, dictIDs, searchRegExp,
                        groupInstances[ groupList->currentIndex() ].id,
                        ignoreDiacritics );
}

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
void MainWindow::toggleMainWindow( bool onlyShow, bool byIconClick )
#else
void MainWindow::toggleMainWindow( bool onlyShow )
#endif
{
  bool shown = false;

  if( !cfg.preferences.searchInDock )
    translateBox->setPopupEnabled( false );

  if ( !isVisible() )
  {
    show();

#ifdef Q_OS_WIN32
    if( !!( hotkeyWrapper ) && hotkeyWrapper->handleViaDLL() )
    {
      // Some dances with tambourine
      HWND wId = (HWND) winId();
      DWORD pId = GetWindowThreadProcessId( wId, NULL );
      DWORD fpId = GetWindowThreadProcessId( GetForegroundWindow(), NULL );

      //Attach Thread to get the Input - i am now allowed to set the Foreground window!
      AttachThreadInput( fpId, pId, true );
      SetActiveWindow( wId );
      SetForegroundWindow( wId );
      SetFocus( wId );
      AttachThreadInput( fpId, pId, false );
    }
#endif

    qApp->setActiveWindow( this );
    activateWindow();
    raise();
    shown = true;
  }
  else
  if ( isMinimized() )
  {
    if( wasMaximized )
      showMaximized();
    else
      showNormal();
    activateWindow();
    raise();
    shown = true;
  }
  else
  if ( !isActiveWindow() )
  {
    qApp->setActiveWindow( this );
#ifdef Q_OS_WIN32
    if( !!( hotkeyWrapper ) && hotkeyWrapper->handleViaDLL() )
    {
      // Some dances with tambourine
      HWND wId = (HWND) winId();
      DWORD pId = GetWindowThreadProcessId( wId, NULL );
      DWORD fpId = GetWindowThreadProcessId( GetForegroundWindow(), NULL );

      //Attach Thread to get the Input - i am now allowed to set the Foreground window!
      AttachThreadInput( fpId, pId, true );
      SetActiveWindow( wId );
      SetForegroundWindow( wId );
      SetFocus( wId );
      AttachThreadInput( fpId, pId, false );
    }
#endif
    raise();
    activateWindow();
    shown = true;
  }
  else
  if ( !onlyShow )
  {
    if (cfg.preferences.enableTrayIcon)
      hide();
    else
      showMinimized();

    if( headwordsDlg )
      headwordsDlg->hide();

    if( ftsDlg )
      ftsDlg->hide();

    if( helpWindow )
      helpWindow->hide();
  }

  if ( shown )
  {
    if( headwordsDlg )
      headwordsDlg->show();

    if( ftsDlg )
      ftsDlg->show();

    focusTranslateLine();

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
    if( !byIconClick )
      QTimer::singleShot( 0, this, SLOT( forceX11Focus() ) );
#endif
  }
}

#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
void MainWindow::forceX11Focus()
{
  Window wh = 0;
  int rev = 0;
  XGetInputFocus( QX11Info::display(), &wh, &rev );
  if( wh != internalWinId() )
  {
    QPoint const pointRelativeToRoot = mapToGlobal( QPoint( 0, 0 ) );
    XEvent event;
    memset( &event, 0, sizeof( event) );
    event.type = ButtonPress;
    event.xbutton.x = 0;
    event.xbutton.y = 0;
    event.xbutton.x_root = pointRelativeToRoot.x();
    event.xbutton.y_root = pointRelativeToRoot.y();
    event.xbutton.window = internalWinId();
    event.xbutton.root = QX11Info::appRootWindow( QX11Info::appScreen() );
    event.xbutton.state = Button1Mask;
    event.xbutton.button = Button1;
    event.xbutton.same_screen = true;
    event.xbutton.time = CurrentTime;

    XSendEvent( QX11Info::display(), internalWinId(), true, 0xfff, &event );
    XFlush( QX11Info::display() );
    event.type = ButtonRelease;
    XSendEvent( QX11Info::display(), internalWinId(), true, 0xfff, &event );
    XFlush( QX11Info::display() );
  }
}
#endif

void MainWindow::installHotKeys()
{
  hotkeyWrapper.reset(); // Remove the old one

  if ( cfg.preferences.enableMainWindowHotkey ||
       cfg.preferences.enableClipboardHotkey )
  {
    try
    {
      hotkeyWrapper = new HotkeyWrapper( this );
    }
    catch( HotkeyWrapper::exInit & )
    {
      QMessageBox::critical( this, "GoldenDict",
        tr( "Failed to initialize hotkeys monitoring mechanism.<br>"
            "Make sure your XServer has RECORD extension turned on." ) );

      return;
    }

    if ( cfg.preferences.enableMainWindowHotkey )
      hotkeyWrapper->setGlobalKey( cfg.preferences.mainWindowHotkey.key1,
                                   cfg.preferences.mainWindowHotkey.key2,
                                   cfg.preferences.mainWindowHotkey.modifiers,
                                   0 );

    if ( cfg.preferences.enableClipboardHotkey && scanPopup.get() )
    {
      hotkeyWrapper->setGlobalKey( cfg.preferences.clipboardHotkey.key1,
                                   cfg.preferences.clipboardHotkey.key2,
                                   cfg.preferences.clipboardHotkey.modifiers,
                                   1 );
    }

    connect( hotkeyWrapper.get(), SIGNAL( hotkeyActivated( int ) ),
             this, SLOT( hotKeyActivated( int ) ),
#ifdef Q_OS_WIN32
             hotkeyWrapper->handleViaDLL() ? Qt::QueuedConnection : Qt::AutoConnection );
#else
             Qt::AutoConnection );
#endif
  }
}

void MainWindow::hotKeyActivated( int hk )
{
  if ( !hk )
    toggleMainWindow();
  else
  if ( scanPopup.get() )
  {
#ifdef HAVE_X11
    // When the user requests translation with the Ctrl+C+C hotkey in certain apps
    // on some GNU/Linux systems, GoldenDict appears to handle Ctrl+C+C before the
    // active application finishes handling Ctrl+C. As a result, GoldenDict finds
    // the clipboard empty, silently cancels the translation request, and users report
    // that Ctrl+C+C is broken in these apps. Slightly delay handling the clipboard
    // hotkey to give the active application more time and thus work around the issue.
    QTimer::singleShot( 10, scanPopup.get(), SLOT( translateWordFromClipboard() ) );
#else
    scanPopup->translateWordFromClipboard();
#endif
  }
}

void MainWindow::prepareNewReleaseChecks()
{
  if ( cfg.preferences.checkForNewReleases )
  {
    QDateTime now = QDateTime::currentDateTime();

    if ( !cfg.timeForNewReleaseCheck.isValid() ||
         now.daysTo( cfg.timeForNewReleaseCheck ) > 2 )
    {
      // The date is invalid, or the check is set to happen more than 2 days
      // in the future -- fix that.
      cfg.timeForNewReleaseCheck = now.addDays( 2 );
    }

    int secsToCheck = now.secsTo( cfg.timeForNewReleaseCheck );

    if ( secsToCheck < 1 )
      secsToCheck = 1;

    newReleaseCheckTimer.setSingleShot( true );
    newReleaseCheckTimer.start( secsToCheck * 1000 );
  }
  else
    newReleaseCheckTimer.stop(); // In case it was started before
}

void MainWindow::checkForNewRelease()
{
  if( latestReleaseReply )
  {
    disconnect( latestReleaseReply, 0, 0, 0 );
    latestReleaseReply->deleteLater();
  }
  latestReleaseReply = 0;

  QNetworkRequest req(
    QUrl( "http://goldendict.org/latest_release.php?current="
          PROGRAM_VERSION "&platform="
#ifdef HAVE_X11
          "x11"
#endif
#ifdef Q_OS_MAC
          "mac"
#endif
#ifdef Q_WS_QWS
          "qws"
#endif
#ifdef Q_OS_WIN
          "win"
#endif
          ) );

  latestReleaseReply = articleNetMgr.get( req );

  connect( latestReleaseReply, SIGNAL( finished() ),
           this, SLOT( latestReleaseReplyReady() ), Qt::QueuedConnection );
}

void MainWindow::latestReleaseReplyReady()
{
  if ( !latestReleaseReply )
    return; // Some stray signal

  bool success = false;
  QString latestVersion, downloadUrl;

  // See if we succeeded

  if ( latestReleaseReply->error() == QNetworkReply::NoError )
  {
    QString latestReleaseInfo = QString::fromUtf8( latestReleaseReply->readLine() ).trimmed();
    QStringList parts = latestReleaseInfo.split( ' ' );
    if ( parts.size() == 2 )
    {
      latestVersion = parts[ 0 ];
      downloadUrl = parts[ 1 ];
      success = true;
    }
  }

  disconnect( latestReleaseReply, 0, 0, 0 );
  latestReleaseReply->deleteLater();
  latestReleaseReply = 0;

  if ( !success )
  {
    // Failed -- reschedule to check in two hours
    newReleaseCheckTimer.start( 2 * 60 * 60 * 1000 );

    GD_DPRINTF( "Failed to check program version, retry in two hours\n" );
  }
  else
  {
    // Success -- reschedule for a normal check and save config
    cfg.timeForNewReleaseCheck = QDateTime();

    prepareNewReleaseChecks();

    Config::save( cfg );

    GD_DPRINTF( "Program version's check successful, current version is %ls\n",
                latestVersion.toStdWString().c_str() );
  }

  if ( success && latestVersion > PROGRAM_VERSION && latestVersion != cfg.skippedRelease )
  {
    QMessageBox msg( QMessageBox::Information,
                     tr( "New Release Available" ),
                     tr( "Version <b>%1</b> of GoldenDict is now available for download.<br>"
                         "Click <b>Download</b> to get to the download page." ).arg( latestVersion ),
                     QMessageBox::NoButton,
                     this );

    QPushButton * dload = msg.addButton( tr( "Download" ), QMessageBox::AcceptRole );
    QPushButton * skip = msg.addButton( tr( "Skip This Release" ), QMessageBox::DestructiveRole );
    msg.addButton( QMessageBox::Cancel );

    msg.exec();

    if ( msg.clickedButton() == dload )
      QDesktopServices::openUrl( QUrl( downloadUrl ) );
    else
    if ( msg.clickedButton() == skip )
    {
      cfg.skippedRelease = latestVersion;
      Config::save( cfg );
    }
  }
}

void MainWindow::trayIconActivated( QSystemTrayIcon::ActivationReason r )
{
  switch(r) {
    case QSystemTrayIcon::Trigger:
      // Left click toggles the visibility of main window
#ifdef X11_MAIN_WINDOW_FOCUS_WORKAROUNDS
      toggleMainWindow( false, true );
#else
      toggleMainWindow();
#endif
      break;

    case QSystemTrayIcon::MiddleClick:
      // Middle mouse click on Tray translates selection
      // it is functional like as stardict
      if ( scanPopup.get() ) {
        scanPopup->translateWordFromSelection();
      }
      break;
    default:
      break;

  }
}

void MainWindow::scanEnableToggled( bool on )
{
  if ( !cfg.preferences.enableScanPopup )
    return;

  if ( scanPopup )
  {
    if ( on )
    {
      scanPopup->enableScanning();
#ifdef Q_OS_MAC
      if( !MacMouseOver::isAXAPIEnabled() )
          mainStatusBar->showMessage( tr( "Accessibility API is not enabled" ), 10000,
                                          QPixmap( ":/icons/error.png" ) );
#endif
    }
    else
      scanPopup->disableScanning();
  }

  updateTrayIcon();
}

void MainWindow::showMainWindow()
{
  toggleMainWindow( true );
}

void MainWindow::visitHomepage()
{
  QDesktopServices::openUrl( QUrl( "http://goldendict.org/" ) );
}

void MainWindow::openConfigFolder()
{
  QDesktopServices::openUrl( QUrl::fromLocalFile( Config::getConfigDir() ) );
}

void MainWindow::visitForum()
{
  QDesktopServices::openUrl( QUrl( "http://goldendict.org/forum/" ) );
}

void MainWindow::showAbout()
{
  About about( this );

  about.show();
  about.exec();
}

void MainWindow::showDictBarNamesTriggered()
{
  bool show = showDictBarNamesAction.isChecked();

  dictionaryBar.setToolButtonStyle( show ? Qt::ToolButtonTextBesideIcon :
                                           Qt::ToolButtonIconOnly );
  cfg.showingDictBarNames = show;
}

void MainWindow::useSmallIconsInToolbarsTriggered()
{
  bool useSmallIcons = useSmallIconsInToolbarsAction.isChecked();

  int extent = useSmallIcons ? QApplication::style()->pixelMetric(QStyle::PM_SmallIconSize) :
                               QApplication::style()->pixelMetric(QStyle::PM_ToolBarIconSize);

  navToolbar->setIconSize( QSize( extent, extent ) );

  // additional fix for #176
  menuButton->setIconSize( QSize( extent, extent ) );

  updateDictionaryBar();

  cfg.usingSmallIconsInToolbars = useSmallIcons;

  if( scanPopup.get() )
    scanPopup->setDictionaryIconSize();
}

void MainWindow::toggleMenuBarTriggered(bool announce)
{
  cfg.preferences.hideMenubar = !toggleMenuBarAction.isChecked();

  if ( announce )
  {
    if ( cfg.preferences.hideMenubar )
    {
      mainStatusBar->showMessage(
            tr( "You have chosen to hide a menubar. Use %1 to show it back." )
            .arg( QString( "<b>%1</b>" ) ).arg( tr( "Ctrl+M" ) ),
            10000,
            QPixmap( ":/icons/warning.png" ) );
    }
    else
    {
      mainStatusBar->clearMessage();
    }
  }

  // Obtain from the menubar all the actions with shortcuts
  // and either add them to the main window or remove them,
  // depending on the menubar state.

  QList<QMenu *> allMenus = menuBar()->findChildren<QMenu *>();
  QListIterator<QMenu *> menuIter( allMenus );
  while( menuIter.hasNext() )
  {
    QMenu * menu = menuIter.next();
    QList<QAction *> allMenuActions = menu->actions();
    QListIterator<QAction *> actionsIter( allMenuActions );
    while( actionsIter.hasNext() )
    {
      QAction * action = actionsIter.next();
      if ( !action->shortcut().isEmpty() )
      {
        if ( cfg.preferences.hideMenubar )
        {
          // add all menubar actions to the main window,
          // before we hide the menubar
          addAction( action );
        }
        else
        {
          // remove all menubar actions from the main window
          removeAction( action );
        }
      }
    }
  }

  menuBar()->setVisible( !cfg.preferences.hideMenubar );
  beforeOptionsSeparator->setVisible( cfg.preferences.hideMenubar);
  menuButtonAction->setVisible( cfg.preferences.hideMenubar );
}

void MainWindow::on_clearHistory_triggered()
{
  history.clear();
  history.save();
}

void MainWindow::on_newTab_triggered()
{
  addNewTab();
}

void MainWindow::setAutostart(bool autostart)
{
#if defined Q_OS_WIN32
    QSettings reg("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                  QSettings::NativeFormat);
    if (autostart) {
        QString app_fname = QString("\"%1\"").arg( QCoreApplication::applicationFilePath() );
        app_fname.replace("/", "\\");
        reg.setValue(QCoreApplication::applicationName(), app_fname);
    }
    else {
        reg.remove(QCoreApplication::applicationName());
    }
    reg.sync();
#elif defined HAVE_X11
  const QString destinationPath = QDir::homePath() + "/.config/autostart/goldendict-owned-by-preferences.desktop";
  if( autostart == QFile::exists( destinationPath ) )
    return; // Nothing to do.
  if( autostart )
  {
    const QString sourcePath = Config::getProgramDataDir() + "../applications/org.goldendict.GoldenDict.desktop";
    QFile::copy( sourcePath, destinationPath );
  }
  else
    QFile::remove( destinationPath );
#endif
}

void MainWindow::on_actionCloseToTray_triggered()
{
  toggleMainWindow( !cfg.preferences.enableTrayIcon );
}

void MainWindow::on_pageSetup_triggered()
{
  if ( getPrinter().isValid() )
  {
    QPageSetupDialog dialog( &getPrinter(), this );

    dialog.exec();
  }
  else
    QMessageBox::critical( this, tr( "Page Setup" ),
                           tr( "No printer is available. Please install one first." ) );
}

void MainWindow::on_printPreview_triggered()
{
  QPrintPreviewDialog dialog( &getPrinter(), this,
                              Qt::WindowSystemMenuHint | Qt::WindowMaximizeButtonHint | Qt::WindowCloseButtonHint );
  dialog.restoreGeometry( cfg.printPreviewDialogGeometry );

  connect( &dialog, SIGNAL( paintRequested( QPrinter * ) ),
           this, SLOT( printPreviewPaintRequested( QPrinter * ) ) );

  dialog.exec();
  cfg.printPreviewDialogGeometry = dialog.saveGeometry();
}

void MainWindow::on_print_triggered()
{
  QPrintDialog dialog( &getPrinter(), this );

  dialog.setWindowTitle( tr( "Print Article") );

  if ( dialog.exec() != QDialog::Accepted )
   return;

  ArticleView *view = getCurrentArticleView();

  view->print( &getPrinter() );
}

void MainWindow::printPreviewPaintRequested( QPrinter * printer )
{
  ArticleView *view = getCurrentArticleView();

  view->print( printer );
}

/// This dialog displays the progress of saving an article. It supports increasing expected
/// number of operations at any time and ensures that the progress bar never moves back.
/// The dialog destroys itself when all operations complete or when canceled.
class ArticleSaveProgressDialog: public QProgressDialog
{
  Q_OBJECT
public:
  explicit ArticleSaveProgressDialog( QWidget * parent = 0 ):
    QProgressDialog( parent ),
    progressStepCount( 100 ),
    operationCount( 1 ), // the first operation is processing and saving the main HTML file
    completedOperationCount( 0 )
  {
    setLabelText( tr( "Saving article..." ) );

    setAutoReset( false );
    setAutoClose( false );

    // Once this modal progress dialog is dismissed, the current tab can be closed and thus the article view destroyed.
    // Destroy the dialog and its child resource saver when canceled to avoid referencing the view and prevent a crash.
    // Also the article view is the parent of ResourceToSaveHandler objects. If a ResourceToSaveHandler object is
    // destroyed along with the view before finishing, it never emits the done() signal. Then completedOperationCount
    // never reaches operationCount, so the progress dialog and its child resource saver are leaked.
    connect( this, SIGNAL( canceled() ), this, SLOT( deleteLater() ) );

    setRange( 0, progressStepCount );
    setValue( 0 );
  }

  void addOperations( int count )
  {
    Q_ASSERT( count > 0 );
    // Forget about already completed operations to reset progress velocity and prevent jumping backwards.
    // This slows progress down, which harms user experience. Can this be improved while keeping the code simple?
    operationCount = operationCount - completedOperationCount + count;
    completedOperationCount = 0;
    progressStepCount = maximum() - value();
  }

public slots:
  void operationCompleted()
  {
    if( wasCanceled() )
      return; // Changing the progress value shows the dialog again. Prevent this by returning early here.

    Q_ASSERT( completedOperationCount < operationCount );
    ++completedOperationCount;

    // Round progress down so that 100% is reached only when all operations complete.
    int const progress = progressStepCount * completedOperationCount / operationCount;
    setValue( maximum() - progressStepCount + progress );

    if( completedOperationCount == operationCount )
    {
      close();
      deleteLater();
    }
  }

private:
  int progressStepCount; ///< The number of available progress steps.
  int operationCount; ///< The number of operations represented by @a progressStepCount.
  int completedOperationCount; ///< The number of completed operations, less or equal to @a operationCount.
};

namespace {

/// Finds custom resource links in an article's HTML, downloads the resources and saves them in the specified resource
/// destination directory. Finds custom resorce links within downloaded code resources recursively, downloads and saves
/// them as well. Replaces each custom resource URL with the relative path to the corresponding saved resource file
/// within the in/out parameter html and within recursively downloaded code resources. Reports progress via the progress
/// dialog argument. The progress dialog becomes the parent of the ArticleResourceSaver object. Thus both the dialog and
/// the ArticleResourceSaver object are destroyed once the article's HTML and all referenced resources are saved or
/// when the dialog is canceled.
class ArticleResourceSaver: public QObject
{
  Q_OBJECT
public:
  explicit ArticleResourceSaver( QString & html, QString const & pathFromHtmlToDestinationDir_,
                                 QString const & resourceDestinationDir_, ArticleView & view_,
                                 ArticleSaveProgressDialog & progressDialog_ ):
    QObject( &progressDialog_ ),
    pathFromHtmlToDestinationDir( pathFromHtmlToDestinationDir_ ),
    resourceDestinationDir( resourceDestinationDir_ ),
    view( view_ ),
    progressDialog( progressDialog_ )
  {
    Q_ASSERT( pathFromHtmlToDestinationDir.isEmpty() || pathFromHtmlToDestinationDir.endsWith( QLatin1Char( '/' ) ) );
    Q_ASSERT( resourceDestinationDir.endsWith( QLatin1Char( '/' ) ) );

    processLinkSource( html, pathFromHtmlToDestinationDir );
  }

private slots:
  void resourceDownloaded( QString const & fileName, QByteArray * resourceData )
  {
    Q_ASSERT( resourceData );

    if( progressDialog.wasCanceled() )
      return; // don't start new downloads after cancelation

    QString pathFromLinkSourceToDestinationDir;
    if( !isLinkSource( fileName, pathFromLinkSourceToDestinationDir ) )
      return; // nothing to do

    QString linkSource = QString::fromUtf8( *resourceData );
    // If the link source is modified, update the resource data before it is saved to a file.
    if( processLinkSource( linkSource, pathFromLinkSourceToDestinationDir ) )
      *resourceData = linkSource.toUtf8();
  }

private:
  QString relativePathFromResourceToDestinationDir( QString const & resourceFileName ) const
  {
    Q_ASSERT( resourceFileName.startsWith( resourceDestinationDir ) );
    int const depthInDestinationDir = resourceFileName.mid( resourceDestinationDir.size() ).count( '/' );

    QString pathToDestinationDir;
    for( int i = 0; i < depthInDestinationDir; ++i )
      pathToDestinationDir += QLatin1String( "../" );
    return pathToDestinationDir;
  }

  bool isLinkSource( QString const & fileName, QString & pathFromLinkSourceToDestinationDir ) const
  {
    if( fileName.endsWith( QLatin1String( ".js" ) ) )
    {
      // Links in JavaScript code replace HTML attribute values, and so are relative to the HTML file location.
      pathFromLinkSourceToDestinationDir = pathFromHtmlToDestinationDir;
      return true;
    }

    if( fileName.endsWith( QLatin1String( ".css" ) ) )
    {
      // Links in a style sheet are relative to the CSS file they are in.
      pathFromLinkSourceToDestinationDir = relativePathFromResourceToDestinationDir( fileName );
      return true;
    }

    return false;
  }

  struct Resource
  {
    explicit Resource( QUrl const & url_, QString const & destinationFilePath_ ):
      url( url_ ), destinationFilePath( destinationFilePath_ )
    {}

    QUrl url;
    QString destinationFilePath;
  };

  /// Finds custom resource links within @p linkSource, determines where the resources should be saved and stores each
  /// unique original resource URL and the corresponding destination path in @p resourcesToDownload. Replaces each
  /// custom resource URL with the corresponding relative destination path within the in/out parameter @p linkSource.
  /// @param rx a regular expression that matches a custom resource URL enclosed in quotes.
  /// @return whether @p linkSource was modified by this function call.
  bool filterAndCollectResources( QString & linkSource, vector< Resource > & resourcesToDownload,
                                  QRegExp const & rx, QString const & pathFromLinkSourceToDestinationDir )
  {
    bool modified = false;
    int pos = 0;
    int queryNom = 1;

    while( ( pos = rx.indexIn( linkSource, pos ) ) != -1 )
    {
      QString urlString = rx.cap();
      Q_ASSERT( urlString.size() > 2 );
      // Remove the enclosing quotes from the match.
      urlString.chop( 1 );
      urlString.remove( 0, 1 );

      QUrl url( urlString );
      if( url.scheme() == QLatin1String( "gdpicture" ) )
        url.setScheme( "bres" );

      QString host = url.host();
      QString resourcePath = Qt4x5::Url::fullPath( url );

#ifdef Q_OS_WIN32
      // Remove the volume separator ':' to make resourcePath a valid subpath.
      if( url.scheme() == QLatin1String( "file" ) && resourcePath.size() > 2
          && resourcePath.at( 0 ) == QLatin1Char( '/' ) && resourcePath.at( 2 ) == QLatin1Char( ':' ) )
      {
        resourcePath.remove( 2, 1 );
      }
#endif

      // Ensure single slash between path components.
      Q_ASSERT( !host.startsWith( QLatin1Char( '/' ) ) );
      Q_ASSERT( !host.endsWith( QLatin1Char( '/' ) ) );
      if( host.isEmpty() )
      {
        if( resourcePath.startsWith( QLatin1Char( '/' ) ) )
          resourcePath.remove( 0, 1 );
      }
      else
      if( !resourcePath.startsWith( '/' ) )
        resourcePath.insert( 0, '/' );

      // Replase query part of url (if exist)
      int n = resourcePath.indexOf( QLatin1Char( '?' ) );
      if( n >= 0 )
      {
        QString q_str = QString( "_q%1" ).arg( queryNom );
        resourcePath.replace( n, resourcePath.length() - n, q_str );
        queryNom += 1;
      }

      QString const pathInDestinationDir = host + resourcePath;
      // Avoid double lookup in encounteredResources.
      int const oldResourceCount = encounteredResources.size();
      encounteredResources.insert( pathInDestinationDir );
      if( encounteredResources.size() != oldResourceCount )
      {
        // This resource was not encountered before => store it in resourcesToDownload.
        resourcesToDownload.push_back( Resource( url, resourceDestinationDir + pathInDestinationDir ) );
      }

      // Modify original url, set to the native one
      resourcePath = QString::fromLatin1( QUrl::toPercentEncoding( resourcePath, "/" ) );
      QString const newUrl = pathFromLinkSourceToDestinationDir + host + resourcePath;
      linkSource.replace( pos + 1, urlString.size(), newUrl ); // keep the enclosing quotes
      modified = true;

      pos += 1 + newUrl.size() + 1; // skip newUrl and the enclosing quotes
    }

    return modified;
  }

  /// See the documentation for the other overload called from this one.
  bool filterAndCollectResources( QString & linkSource, vector< Resource > & resourcesToDownload,
                                  QString const & pathFromLinkSourceToDestinationDir )
  {
    static QRegExp const rx1( "'(?:bres|gdpicture|gico|gdau|gdvideo|qrcx?|file)://[^']+'" );
    static QRegExp const rx2( rx1.pattern().replace( '\'', '"' ) );

    bool const modified1 = filterAndCollectResources( linkSource, resourcesToDownload, rx1,
                                                      pathFromLinkSourceToDestinationDir );
    bool const modified2 = filterAndCollectResources( linkSource, resourcesToDownload, rx2,
                                                      pathFromLinkSourceToDestinationDir );

    return modified1 || modified2;
  }

  /// @return whether @p linkSource was modified by this function call.
  bool processLinkSource( QString & linkSource, QString const & pathFromLinkSourceToDestinationDir )
  {
    vector< Resource > resourcesToDownload;
    bool const modified = filterAndCollectResources( linkSource, resourcesToDownload,
                                                     pathFromLinkSourceToDestinationDir );
    int asyncSavedResources = 0;

    // Pull and save resources to files
    for( vector< Resource >::const_iterator it = resourcesToDownload.begin(); it != resourcesToDownload.end(); ++it )
    {
      ResourceToSaveHandler * const handler = new ResourceToSaveHandler( &view, it->destinationFilePath );

      // handler may emit downloaded() synchronously from ArticleView::saveResource() => connect to it now.
      connect( handler, SIGNAL( downloaded( QString, QByteArray * ) ),
               this, SLOT( resourceDownloaded( QString, QByteArray * ) ) );

      view.saveResource( it->url, *handler );
      if( !handler->isEmpty() )
      {
        ++asyncSavedResources;
        connect( handler, SIGNAL( done() ), &progressDialog, SLOT( operationCompleted() ) );
      }
      // else: the resource was downloaded and saved synchronously => it should not affect the progress dialog.
    }

    if( asyncSavedResources != 0)
      progressDialog.addOperations( asyncSavedResources );

    return modified;
  }

  QString const pathFromHtmlToDestinationDir;
  QString const resourceDestinationDir;
  ArticleView & view;
  ArticleSaveProgressDialog & progressDialog;

  QSet< QString > encounteredResources; ///< Contains destination subpaths of all encountered custom resource files.
};

void insertSavedArticleScript( QString & html )
{
  QLatin1String const insertBeforeString( "<script" );
  int const pos = html.indexOf( insertBeforeString );
  if( pos == -1 )
  {
    gdWarning( "Couldn't find \"%s\" in an article's HTML code.", insertBeforeString.latin1() );
    return;
  }

#ifdef USE_QTWEBKIT
#define w "webkit"
#else
#define w "webengine"
#endif
  html.insert( pos, QLatin1String( "<script src='qrc:///scripts/" w "_saved_article.js'></script>" ) );
#undef w
}

} // unnamed namespace

void MainWindow::on_saveArticle_triggered()
{
  ArticleView *view = getCurrentArticleView();

  QString fileName = view->getTitle().simplified();

  // Replace reserved filename characters
  QRegExp rxName( "[/\\\\\\?\\*:\\|<>]" );
  fileName.replace( rxName, "_" );

  fileName += ".html";
  QString savePath;

  if ( cfg.articleSavePath.isEmpty() )
    savePath = QDir::homePath();
  else
  {
    savePath = QDir::fromNativeSeparators( cfg.articleSavePath );
    if ( !QDir( savePath ).exists() )
      savePath = QDir::homePath();
  }

  QFileDialog::Options options = QFileDialog::HideNameFilterDetails;
  QString selectedFilter;
  QStringList filters;
  filters.push_back( tr( "Article, Complete (*.html)" ) );
  filters.push_back( tr( "Article, HTML Only (*.html)" ) );

  fileName = savePath + "/" + fileName;
  fileName = QFileDialog::getSaveFileName( this, tr(  "Save Article As" ),
                                           fileName,
                                           filters.join( ";;" ),
                                           &selectedFilter, options );

  // The " (*.html)" part of filters[i] is absent from selectedFilter in Qt 5.
  bool const complete = filters.at( 0 ).startsWith( selectedFilter );

  if( fileName.isEmpty() )
    return;

#ifdef USE_QTWEBKIT
  QString html = view->toHtml();
  saveArticleAs( *view, html, fileName, complete, rxName, 0 );
#else
  // QWebEnginePage::save() doesn't even save qrc resources, not to mention custom URL scheme resources.
  // Our custom page-saving implementation saves a complete web page correctly.

  auto * const progressDialog = new ArticleSaveProgressDialog( this );
  progressDialog->show();

  view->toHtml( [ this, view, fileName, complete, rxName,
                  progressDialog = QPointer< ArticleSaveProgressDialog >{ progressDialog } ]( QString const & result ) {
    if( result.isEmpty() || !progressDialog || progressDialog->wasCanceled() )
    {
      // This callback is being called during page destruction or the user canceled the saving of the page.
      return; // return now to prevent a crash
    }
    QString html = result;
    saveArticleAs( *view, html, fileName, complete, rxName, progressDialog );
  } );
#endif
}

void MainWindow::saveArticleAs( ArticleView & view, QString & html, QString const & fileName, bool complete,
                                QRegExp const & rxName, ArticleSaveProgressDialog * progressDialog )
{
  QFile file( fileName );
  if ( !file.open( QIODevice::WriteOnly ) )
  {
    QMessageBox::critical( this, tr( "Error" ),
                           tr( "Can't save article: %1" ).arg( file.errorString() ) );
    return;
  }

  QFileInfo fi( fileName );
  cfg.articleSavePath = QDir::toNativeSeparators( fi.absoluteDir().absolutePath() );

  // Convert internal links

  QRegExp rx3( "href=\"(bword:|gdlookup://localhost/)([^\"]+)\"" );
  int pos = 0;
  while ( ( pos = rx3.indexIn( html, pos ) ) != -1 )
  {
    QString name = QUrl::fromPercentEncoding( rx3.cap( 2 ).simplified().toLatin1() );
    QString anchor;
    name.replace( "?gdanchor=", "#" );
    int n = name.indexOf( '#' );
    if( n > 0 )
    {
      anchor = name.mid( n );
      name.truncate( n );
      anchor.replace( QRegExp( "(g[0-9a-f]{32}_)[0-9a-f]+_" ), "\\1" ); // MDict anchors
    }
    name.replace( rxName, "_" );
    name = QString( "href=\"" ) + QUrl::toPercentEncoding( name ) + ".html" + anchor + "\"";
    html.replace( pos, rx3.cap().length(), name );
    pos += name.length();
  }

  // MDict anchors
  QRegExp anchorLinkRe( "(<\\s*a\\s+[^>]*\\b(?:name|id)\\b\\s*=\\s*[\"']*g[0-9a-f]{32}_)([0-9a-f]+_)(?=[^\"'])", Qt::CaseInsensitive );
  html.replace( anchorLinkRe, "\\1" );

#ifdef USE_QTWEBKIT
  Q_ASSERT( !progressDialog );

  if( !complete )
  {
    file.write( html.toUtf8() );
    return;
  }

  progressDialog = new ArticleSaveProgressDialog( this );
  progressDialog->show();
#else
  Q_ASSERT( progressDialog );

  if( complete )
#endif
  {
    insertSavedArticleScript( html );

    QString pathFromHtmlToDestinationDir = fi.baseName() + "_files/";
    QString resourceDestinationDir = fi.absoluteDir().absolutePath() + '/' + pathFromHtmlToDestinationDir;
    new ArticleResourceSaver( html, pathFromHtmlToDestinationDir, resourceDestinationDir, view, *progressDialog );
  }

  file.write( html.toUtf8() );
  progressDialog->operationCompleted();
}

void MainWindow::on_rescanFiles_triggered()
{
  hotkeyWrapper.reset(); // No hotkeys while we're editing dictionaries
  scanPopup.reset(); // No scan popup either. No one should use dictionaries.
  closeHeadwordsDialog();
  closeFullTextSearchDialog();

  ftsIndexing.stopIndexing();
  ftsIndexing.clearDictionaries();

  groupInstances.clear(); // Release all the dictionaries they hold
  dictionaries.clear();
  dictionariesUnmuted.clear();
  dictionaryBar.setDictionaries( dictionaries );

  loadDictionaries( this, true, cfg, dictionaries, dictNetMgr );

  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    dictionaries[ x ]->setFTSParameters( cfg.preferences.fts );
    dictionaries[ x ]->setSynonymSearchEnabled( cfg.preferences.synonymSearchEnabled );
  }

  ftsIndexing.setDictionaries( dictionaries );
  ftsIndexing.doIndexing();

  updateGroupList();

  makeScanPopup();
  installHotKeys();

  updateSuggestionList();
}

void MainWindow::on_alwaysOnTop_triggered( bool checked )
{
    cfg.preferences.alwaysOnTop = checked;

    bool wasVisible = isVisible();

    Qt::WindowFlags flags = this->windowFlags();
    if (checked)
    {
        setWindowFlags(flags | Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint);
        mainStatusBar->showMessage(
              tr( "The main window is set to be always on top." ),
              10000,
              QPixmap( ":/icons/warning.png" ) );
    }
    else
    {
        setWindowFlags(flags ^ (Qt::CustomizeWindowHint | Qt::WindowStaysOnTopHint));
        mainStatusBar->clearMessage();
    }

    if ( wasVisible )
    {
      show();
    }

    installHotKeys();
}

void MainWindow::zoomin()
{
  cfg.preferences.zoomFactor += 0.1;
  applyZoomFactor();
}

void MainWindow::zoomout()
{
  cfg.preferences.zoomFactor -= 0.1;
  applyZoomFactor();
}

void MainWindow::unzoom()
{
  cfg.preferences.zoomFactor = 1;
  applyZoomFactor();
}

void MainWindow::applyZoomFactor()
{
  // Always call this function synchronously to potentially disable a zoom action,
  // which is being repeatedly triggered. When the action is disabled, its
  // triggered() signal is no longer emitted, which in turn improves performance.
  adjustCurrentZoomFactor();

#if defined( USE_QTWEBKIT ) && QT_VERSION >= QT_VERSION_CHECK( 5, 0, 0 )
  // Scaling article views asynchronously dramatically improves performance when
  // a zoom action is triggered repeatedly while many or large articles are open
  // in the main window or in scan popup.
  // Multiple zoom action signals are processed before (often slow) article view
  // scaling is requested. Multiple scaling requests then ask for the same zoom factor,
  // so all of them except for the first one don't change anything and run very fast.
  // In effect, some intermediate zoom factors are skipped when scaling is slow.
  // The slower the scaling, the more steps are skipped.

  // Unfortunately this optimization does not work in the Qt WebEngine version, where
  // the UI does not completely freeze (e.g. scrolling the page still works) and events
  // keep being delivered even as the page is being scaled in a separate process.
  // This issue must have been fixed upstream in Qt 6.4.0 or earlier, because repeated zooming is slow in
  // Qt WebEngine Widgets Simple Browser Example version 5.15.10 and fast in the same example version 6.4.0.

  QTimer::singleShot( 0, this, SLOT( scaleArticlesByCurrentZoomFactor() ) );
#else
  // The timer trick above does not help in unfixed Qt WebEngine versions and is not needed in fixed ones.

  // The timer trick above usually doesn't improve performance with Qt4
  // due to a different ordering of keyboard and timer events.
  // Sometimes, unpredictably, it does work like with Qt5.
  // Scale article views synchronously to avoid inconsistent or unexpected behavior.

  scaleArticlesByCurrentZoomFactor();
#endif
}

void MainWindow::adjustCurrentZoomFactor()
{
  if ( cfg.preferences.zoomFactor >= 5 )
    cfg.preferences.zoomFactor = 5;
  else if ( cfg.preferences.zoomFactor <= 0.1 )
    cfg.preferences.zoomFactor = 0.1;

  zoomIn->setEnabled( cfg.preferences.zoomFactor < 5 );
  zoomOut->setEnabled( cfg.preferences.zoomFactor > 0.1 );
  zoomBase->setEnabled( cfg.preferences.zoomFactor != 1.0 );
}

void MainWindow::scaleArticlesByCurrentZoomFactor()
{
  for ( int i = 0; i < ui.tabWidget->count(); i++ )
  {
    ArticleView & view =
      dynamic_cast< ArticleView & >( *( ui.tabWidget->widget(i) ) );
    view.setZoomFactor( cfg.preferences.zoomFactor );
  }

  if ( scanPopup.get() )
    scanPopup->applyZoomFactor();
}

void MainWindow::doWordsZoomIn()
{
  ++cfg.preferences.wordsZoomLevel;

  applyWordsZoomLevel();
}

void MainWindow::doWordsZoomOut()
{
  --cfg.preferences.wordsZoomLevel;

  applyWordsZoomLevel();
}

void MainWindow::doWordsZoomBase()
{
  cfg.preferences.wordsZoomLevel = 0;

  applyWordsZoomLevel();
}

void MainWindow::applyWordsZoomLevel()
{
  QFont font( wordListDefaultFont );

  int ps = font.pointSize();

  if ( cfg.preferences.wordsZoomLevel != 0 )
  {
    ps += cfg.preferences.wordsZoomLevel;

    if ( ps < 1 )
      ps = 1;

    font.setPointSize( ps );
  }

  if ( wordList->font().pointSize() != ps )
    wordList->setFont( font );

  font = translateLineDefaultFont;

  ps = font.pointSize();

  if ( cfg.preferences.wordsZoomLevel != 0 )
  {
    ps += cfg.preferences.wordsZoomLevel;

    if ( ps < 1 )
      ps = 1;

    font.setPointSize( ps );
  }

  if ( translateLine->font().pointSize() != ps )
  {
    translateLine->setFont( font );
    if( cfg.preferences.searchInDock )
    {
      // loadingIndicatorLabel's size hint is bound to ui.translateLine->height(), which depends on the font.
      // Update the label's geometry to resize it immediately rather than after it is hidden and shown again.
      ui.loadingIndicatorLabel->updateGeometry();
    }
  }

  font = groupListDefaultFont;

  ps = font.pointSize();

  if ( cfg.preferences.wordsZoomLevel != 0 )
  {
    ps += cfg.preferences.wordsZoomLevel;

    if ( ps < 1 )
      ps = 1;

    font.setPointSize( ps );
  }

  if ( groupList->font().pointSize() != ps )
  {
    disconnect( groupList, SIGNAL( currentIndexChanged( QString const & ) ),
                this, SLOT( currentGroupChanged( QString const & ) ) );
    int n = groupList->currentIndex();
    groupList->clear();
    groupList->setFont( font );
    groupList->fill( groupInstances );
    groupList->setCurrentIndex( n );
    connect( groupList, SIGNAL( currentIndexChanged( QString const & ) ),
             this, SLOT( currentGroupChanged( QString const & ) ) );
  }

  wordsZoomBase->setEnabled( cfg.preferences.wordsZoomLevel != 0 );

  if( !cfg.preferences.searchInDock )
  {
    // Invalidating navToolbar's layout displays translateBoxWidget w/o the need to press the toolbar
    // extension button when Words Zoom level decreases enough for translateBoxWidget to fit in the toolbar.
    navToolbar->layout()->invalidate();
  }

  if ( scanPopup.get() )
    scanPopup->applyWordsZoomLevel();
}

void MainWindow::messageFromAnotherInstanceReceived( QString const & message )
{
  if ( message == "bringToFront" )
  {
    toggleMainWindow( true );
    return;
  }
  if( message.left( 15 ) == "translateWord: " )
  {
    if( scanPopup.get() )
      scanPopup->translateWord( message.mid( 15 ) );
    else
      wordReceived( message.mid( 15 ) );
  }
  else
  if( message.left( 10 ) == "setGroup: " )
  {
    setGroupByName( message.mid( 10 ), true );
  }
  else
  if( message.left( 15 ) == "setPopupGroup: " )
  {
    setGroupByName( message.mid( 15 ), false );
  }
  else
    qWarning() << "Unknown message received from another instance: " << message;
}

ArticleView * MainWindow::getCurrentArticleView()
{
  if ( QWidget * cw = ui.tabWidget->currentWidget() )
  {
    return dynamic_cast< ArticleView * >( cw );
  }
  return 0;
}

void MainWindow::phraseReceived( Config::InputPhrase const & phrase )
{
  toggleMainWindow( true );
  setTranslateBoxTextAndKeepSuffix( phrase.phrase, EscapeWildcards, NoPopupChange );
  translateBoxSuffix = phrase.punctuationSuffix;
  respondToTranslationRequest( phrase, false );
}

void MainWindow::wordReceived( const QString & word)
{
  phraseReceived( Config::InputPhrase::fromPhrase( word ) );
}

void MainWindow::headwordReceived( const QString & word, const QString & ID )
{
  toggleMainWindow( true );
  setTranslateBoxTextAndClearSuffix( word, EscapeWildcards, NoPopupChange );
  respondToTranslationRequest( Config::InputPhrase::fromPhrase( word ),
                               false, ArticleView::scrollToFromDictionaryId( ID ) );
}

void MainWindow::updateFavoritesMenu()
{
  if ( ui.favoritesPane->toggleViewAction()->isChecked() )
  {
    ui.showHideFavorites->setText( tr( "&Hide" ) );
  }
  else
  {
    ui.showHideFavorites->setText( tr( "&Show" ) );
  }
}

void MainWindow::updateHistoryMenu()
{
  if ( ui.historyPane->toggleViewAction()->isChecked() )
  {
    ui.showHideHistory->setText( tr( "&Hide" ) );
  }
  else
  {
    ui.showHideHistory->setText( tr( "&Show" ) );
  }
}

void MainWindow::on_showHideFavorites_triggered()
{
  ui.favoritesPane->toggleViewAction()->trigger();
  ui.favoritesPane->raise(); // useful when the Pane is tabbed.
}

void MainWindow::on_showHideHistory_triggered()
{
  ui.historyPane->toggleViewAction()->trigger();
  ui.historyPane->raise(); // useful when the Pane is tabbed.
}

void MainWindow::on_exportHistory_triggered()
{
    QString exportPath;
    if( cfg.historyExportPath.isEmpty() )
        exportPath = QDir::homePath();
    else
    {
        exportPath = QDir::fromNativeSeparators( cfg.historyExportPath );
        if( !QDir( exportPath ).exists() )
            exportPath = QDir::homePath();
    }

    QString fileName = QFileDialog::getSaveFileName( this, tr( "Export history to file" ),
                                                     exportPath,
                                                     tr( "Text files (*.txt);;All files (*.*)" ) );
    if( fileName.size() == 0)
        return;

    cfg.historyExportPath = QDir::toNativeSeparators( QFileInfo( fileName ).absoluteDir().absolutePath() );
    QFile file( fileName );

    for(;;)
    {
        if ( !file.open( QFile::WriteOnly | QIODevice::Text ) )
          break;

        // Write UTF-8 BOM
        QByteArray line;
        line.append( 0xEF ).append( 0xBB ).append( 0xBF );
        if ( file.write( line ) != line.size() )
          break;

        // Write history
        QList< History::Item > const & items = history.getItems();

        QList< History::Item >::const_iterator i;
        for( i = items.constBegin(); i != items.constEnd(); ++i )
        {
          line = i->word.toUtf8();

          line.replace( '\n', ' ' );
          line.replace( '\r', ' ' );

          line += "\n";

          if ( file.write( line ) != line.size() )
            break;
        }

        if( i != items.constEnd() )
          break;

        file.close();
        mainStatusBar->showMessage( tr( "History export complete" ), 5000 );
        return;
    }
    QString errStr = QString( tr( "Export error: " ) ) + file.errorString();
    file.close();
    mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
}

// TODO: consider moving parts of this method into History class.
void MainWindow::on_importHistory_triggered()
{
    QString importPath;
    if( cfg.historyExportPath.isEmpty() )
        importPath = QDir::homePath();
    else
    {
        importPath = QDir::fromNativeSeparators( cfg.historyExportPath );
        if( !QDir( importPath ).exists() )
            importPath = QDir::homePath();
    }

    QString fileName = QFileDialog::getOpenFileName( this, tr( "Import history from file" ),
                                                     importPath,
                                                     tr( "Text files (*.txt);;All files (*.*)" ) );
    if( fileName.size() == 0)
        return;

    QFileInfo fileInfo( fileName );
    cfg.historyExportPath = QDir::toNativeSeparators( fileInfo.absoluteDir().absolutePath() );
    QString errStr;
    QFile file( fileName );

    for(;;)
    {
        if ( !file.open( QFile::ReadOnly | QIODevice::Text ) )
          break;

        QTextStream fileStream( & file );
        QString itemStr, trimmedStr;
        QList< QString > itemList;

        history.clear();

        do
        {
            itemStr = fileStream.readLine();
            if( fileStream.status() >= QTextStream::ReadCorruptData )
                break;

            trimmedStr = itemStr.trimmed();
            if( trimmedStr.isEmpty() )
                continue;

            if( (unsigned)trimmedStr.size() <= history.getMaxItemLength( ) )
                itemList.prepend( trimmedStr );

        } while( !fileStream.atEnd() && itemList.size() < (int)history.getMaxSize() );

        history.enableAdd( true );

        for( QList< QString >::const_iterator i = itemList.constBegin(); i != itemList.constEnd(); ++i )
            history.addItem( History::Item( 1, *i ) );

        history.enableAdd( cfg.preferences.storeHistory );

        if( file.error() != QFile::NoError )
            break;

        if( fileStream.status() >= QTextStream::ReadCorruptData )
        {
            errStr = QString ( tr( "Import error: invalid data in file" ) );
            mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
        }
        else
            mainStatusBar->showMessage( tr( "History import complete" ), 5000 );
        return;
    }
    errStr = QString( tr( "Import error: " ) ) + file.errorString();
    file.close();
    mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
}

void MainWindow::on_exportFavorites_triggered()
{
  QString exportPath;
  if( cfg.historyExportPath.isEmpty() )
    exportPath = QDir::homePath();
  else
  {
    exportPath = QDir::fromNativeSeparators( cfg.historyExportPath );
    if( !QDir( exportPath ).exists() )
      exportPath = QDir::homePath();
  }

  QString fileName = QFileDialog::getSaveFileName( this, tr( "Export Favorites to file" ),
                                                   exportPath,
                                                   tr( "XML files (*.xml);;All files (*.*)" ) );
  if( fileName.size() == 0)
    return;

  cfg.historyExportPath = QDir::toNativeSeparators( QFileInfo( fileName ).absoluteDir().absolutePath() );
  QFile file( fileName );

  for(;;)
  {
    if ( !file.open( QFile::WriteOnly | QIODevice::Text ) )
      break;

    QByteArray data;
    ui.favoritesPaneWidget->getDataInXml( data );

    if( file.write( data ) != data.size() )
      break;

    file.close();
    mainStatusBar->showMessage( tr( "Favorites export complete" ), 5000 );
    return;
  }
  QString errStr = QString( tr( "Export error: " ) ) + file.errorString();
  file.close();
  mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
}

void MainWindow::on_ExportFavoritesToList_triggered()
{
  QString exportPath;
  if( cfg.historyExportPath.isEmpty() )
    exportPath = QDir::homePath();
  else
  {
    exportPath = QDir::fromNativeSeparators( cfg.historyExportPath );
    if( !QDir( exportPath ).exists() )
      exportPath = QDir::homePath();
  }

  QString fileName = QFileDialog::getSaveFileName( this, tr( "Export Favorites to file as plain list" ),
                                                   exportPath,
                                                   tr( "Text files (*.txt);;All files (*.*)" ) );
  if( fileName.size() == 0)
    return;

  cfg.historyExportPath = QDir::toNativeSeparators( QFileInfo( fileName ).absoluteDir().absolutePath() );
  QFile file( fileName );

  for(;;)
  {
    if ( !file.open( QFile::WriteOnly | QIODevice::Text ) )
      break;

    // Write UTF-8 BOM
    QByteArray line;
    line.append( 0xEF ).append( 0xBB ).append( 0xBF );
    if ( file.write( line ) != line.size() )
      break;

    // Write Favorites
    QString data;
    ui.favoritesPaneWidget->getDataInPlainText( data );

    line = data.toUtf8();
    if( file.write( line ) != line.size() )
      break;

    file.close();
    mainStatusBar->showMessage( tr( "Favorites export complete" ), 5000 );
    return;
  }
  QString errStr = QString( tr( "Export error: " ) ) + file.errorString();
  file.close();
  mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
}

void MainWindow::on_importFavorites_triggered()
{
  QString importPath;
  if( cfg.historyExportPath.isEmpty() )
    importPath = QDir::homePath();
  else
  {
    importPath = QDir::fromNativeSeparators( cfg.historyExportPath );
    if( !QDir( importPath ).exists() )
      importPath = QDir::homePath();
  }

  QString fileName = QFileDialog::getOpenFileName( this, tr( "Import Favorites from file" ),
                                                   importPath,
                                                   tr( "XML files (*.xml);;All files (*.*)" ) );
  if( fileName.size() == 0)
    return;

  QFileInfo fileInfo( fileName );
  cfg.historyExportPath = QDir::toNativeSeparators( fileInfo.absoluteDir().absolutePath() );
  QString errStr;
  QFile file( fileName );

  for(;;)
  {
    if ( !file.open( QFile::ReadOnly | QIODevice::Text ) )
      break;

    if( file.error() != QFile::NoError )
        break;

    QByteArray data = file.readAll();

    if( !ui.favoritesPaneWidget->setDataFromXml( QString::fromUtf8( data.data(), data.size() ) ) )
      break;

    file.close();
    mainStatusBar->showMessage( tr( "Favorites import complete" ), 5000 );
    return;
  }
  if( file.error() != QFile::NoError )
    errStr = QString( tr( "Import error: " ) ) + file.errorString();
  else
    errStr = QString( tr( "Data parsing error" ) );

  file.close();
  mainStatusBar->showMessage( errStr, 10000, QPixmap( ":/icons/error.png" ) );
}

void MainWindow::fillWordListFromHistory()
{
    ui.wordList->setUpdatesEnabled( false );
    ui.wordList->clear();

    QList< History::Item > const & items = history.getItems();
    for( int x = 0; x < items.size(); ++x )
    {
      History::Item const * i = &items[ x ];
      QListWidgetItem * s = new QListWidgetItem( i->word, ui.wordList );
      if (s->text().at(0).direction() == QChar::DirR)
          s->setTextAlignment(Qt::AlignRight);
      if (s->text().at(0).direction() == QChar::DirL)
          s->setTextAlignment(Qt::AlignLeft);
      ui.wordList->addItem( s );
    }

    ui.wordList->setUpdatesEnabled( true );
}

void MainWindow::focusWordList()
{
    if( ui.wordList->count() > 0 )
        ui.wordList->setFocus();
}

void MainWindow::addWordToHistory( const QString & word )
{
  history.addItem( History::Item( 1, word.trimmed() ) );
}

void MainWindow::forceAddWordToHistory( const QString & word )
{
    history.enableAdd( true );
    history.addItem( History::Item( 1, word.trimmed() ) );
    history.enableAdd( cfg.preferences.storeHistory );
}

void MainWindow::setExpandMode( bool expand )
{
  articleMaker.setExpandOptionalParts( expand );
}

void MainWindow::switchExpandOptionalPartsMode()
{
  ArticleView * view = getCurrentArticleView();
  if( view )
    view->switchExpandOptionalParts();
}

void MainWindow::foundDictsPaneClicked( QListWidgetItem * item )
{
  Qt::KeyboardModifiers m = QApplication::keyboardModifiers();
  if ( ( m & ( Qt::ControlModifier | Qt::ShiftModifier ) )
       || ( m == Qt::AltModifier ) )
  {
    QString id = item->data( Qt::UserRole ).toString();
    emit clickOnDictPane( id );
  }
}

void MainWindow::showDictionaryInfo( const QString & id )
{
  QWidget * owner = 0;

  if( sender()->objectName().compare( "EditDictionaries" ) == 0 )
    owner = qobject_cast< QWidget * >( sender() );

  if( owner == 0 )
    owner = this;

  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    if( dictionaries[ x ]->getId() == id.toUtf8().data() )
    {
      DictInfo infoMsg( cfg, this );
      infoMsg.showInfo( dictionaries[ x ] );
      int result = infoMsg.exec();

      if ( result == DictInfo::OPEN_FOLDER )
      {
        openDictionaryFolder( id );
      }
      else if ( result == DictInfo::EDIT_DICTIONARY)
      {
        editDictionary( dictionaries[x].get() );
      }
      else if( result == DictInfo::SHOW_HEADWORDS )
      {
        showDictionaryHeadwords( owner, dictionaries[x].get() );
      }

      break;
    }
  }
}

void MainWindow::showDictionaryHeadwords( const QString & id )
{
  QWidget * owner = 0;

  if( sender()->objectName().compare( "EditDictionaries" ) == 0 )
    owner = qobject_cast< QWidget * >( sender() );

  if( owner == 0 )
    owner = this;

  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    if( dictionaries[ x ]->getId() == id.toUtf8().data() )
    {
      showDictionaryHeadwords( owner, dictionaries[ x ].get() );
      break;
    }
  }
}

void MainWindow::showDictionaryHeadwords( QWidget * owner, Dictionary::Class * dict )
{
  if( owner && owner != this )
  {
    DictHeadwords headwords( owner, cfg, dict );
    headwords.exec();
    return;
  }

  if( headwordsDlg == 0 )
  {
    headwordsDlg = new DictHeadwords( this, cfg, dict );
    addGlobalActionsToDialog( headwordsDlg );
    addGroupComboBoxActionsToDialog( headwordsDlg, groupList );
    connect( headwordsDlg, SIGNAL( headwordSelected( QString, QString ) ),
             this, SLOT( headwordReceived( QString, QString ) ) );
    connect( headwordsDlg, SIGNAL( closeDialog() ),
             this, SLOT( closeHeadwordsDialog() ), Qt::QueuedConnection );
  }
  else
    headwordsDlg->setup( dict );

  headwordsDlg->show();
}

void MainWindow::closeHeadwordsDialog()
{
  if( headwordsDlg )
  {
    delete headwordsDlg;
    headwordsDlg = NULL;
  }
}

void MainWindow::focusHeadwordsDialog()
{
  if( headwordsDlg )
  {
    headwordsDlg->activateWindow();
    if ( ftsDlg )
      ftsDlg->lower();
  }
}

void MainWindow::focusArticleView()
{
  ArticleView * view = getCurrentArticleView();
  if ( view )
  {
    if ( !isActiveWindow() )
      activateWindow();
    view->focus();
  }
}

void MainWindow::editDictionary( Dictionary::Class * dict )
{
  QString dictFilename = dict->getMainFilename();
  if( !cfg.editDictionaryCommandLine.isEmpty() && !dictFilename.isEmpty() )
  {
    QString command( cfg.editDictionaryCommandLine );
    command.replace( "%GDDICT%", "\"" + dictFilename + "\"" );
    if( command.contains( "%GDWORD%" ) )
    {
      QString headword = unescapeTabHeader( ui.tabWidget->tabText( ui.tabWidget->currentIndex() ) );
      command.replace( "%GDWORD%", headword );
    }
    if( !QProcess::startDetached( command ) )
      QApplication::beep();
  }
}


void MainWindow::openDictionaryFolder( const QString & id )
{
  for( unsigned x = 0; x < dictionaries.size(); x++ )
  {
    if( dictionaries[ x ]->getId() == id.toUtf8().data() )
    {
      if( dictionaries[ x ]->getDictionaryFilenames().size() > 0 )
      {
        QString fileName = FsEncoding::decode( dictionaries[ x ]->getDictionaryFilenames()[ 0 ].c_str() );
        bool explorerLaunched = false;

        // Platform-dependent way to launch a file explorer and to select a file,
        // currently only on Windows.
#if defined(Q_OS_WIN)
        if ( !QFileInfo( fileName ).isDir() )
        {
          QString param = QLatin1String("explorer.exe ")
              + QLatin1String("/select, \"") + QDir::toNativeSeparators( fileName ) + QLatin1String("\"");

          qDebug() << "Launching" << param;

          // We use CreateProcess() directly instead of QProcess::startDetached() since
          // startDetached() does some evil things with quotes breaking explorer arguments.
          // E.g., the following file cannot be properly selected via startDetached(), due to equals sign,
          // which explorer considers as separator:
          // Z:\GoldenDict\content\-=MDict=-\Test.mdx
          PROCESS_INFORMATION pinfo;
          STARTUPINFOW startupInfo = { sizeof( STARTUPINFO ), 0, 0, 0,
                                       (ulong)CW_USEDEFAULT, (ulong)CW_USEDEFAULT,
                                       (ulong)CW_USEDEFAULT, (ulong)CW_USEDEFAULT,
                                       0, 0, 0, 0, 0, 0, 0, 0, 0, 0
                                     };
          explorerLaunched = CreateProcess(0, (wchar_t*) param.utf16(),
                                           0, 0, FALSE, CREATE_UNICODE_ENVIRONMENT | CREATE_NEW_CONSOLE, 0,
                                           0, &startupInfo, &pinfo);

          if ( explorerLaunched ) {
            CloseHandle( pinfo.hThread );
            CloseHandle( pinfo.hProcess );
          }
        }
#endif

        if ( !explorerLaunched )
        {
          QString folder = QFileInfo( fileName ).absoluteDir().absolutePath();
          if( !folder.isEmpty() )
            QDesktopServices::openUrl( QUrl::fromLocalFile( folder ) );
        }
      }
      break;
    }
  }
}

void MainWindow::foundDictsContextMenuRequested( const QPoint &pos )
{
  QListWidgetItem *item = ui.dictsList->itemAt( pos );
  if( item )
  {
    QString id = item->data( Qt::UserRole ).toString();
    Dictionary::Class *pDict = NULL;

    for( unsigned i = 0; i < dictionaries.size(); i++ )
    {
      if( id.compare( dictionaries[ i ]->getId().c_str() ) == 0 )
      {
        pDict = dictionaries[ i ].get();
        break;
      }
    }

    if( pDict == NULL )
      return;

    if( !pDict->isLocalDictionary() )
    {
      if ( scanPopup )
        scanPopup.get()->blockSignals( true );
      showDictionaryInfo( id );
      if ( scanPopup )
        scanPopup.get()->blockSignals( false );
    }
    else
    {
      QMenu menu( ui.dictsList );
      QAction * infoAction = menu.addAction( tr( "Dictionary info" ) );

      QAction * headwordsAction = NULL;
      if( pDict->getWordCount() > 0 )
        headwordsAction = menu.addAction( tr( "Dictionary headwords" ) );

      QAction * openDictFolderAction = menu.addAction( tr( "Open dictionary folder" ) );

      QAction * editAction = NULL;

      QString dictFilename = pDict->getMainFilename();
      if( !cfg.editDictionaryCommandLine.isEmpty() && !dictFilename.isEmpty() )
        editAction = menu.addAction( tr( "Edit dictionary" ) );

      QAction * result = menu.exec( ui.dictsList->mapToGlobal( pos ) );

      if( result && result == infoAction )
      {
        if ( scanPopup )
          scanPopup.get()->blockSignals( true );
        showDictionaryInfo( id );
        if ( scanPopup )
          scanPopup.get()->blockSignals( false );
      }
      else
      if( result && result == headwordsAction )
      {
        if ( scanPopup )
          scanPopup.get()->blockSignals( true );
        showDictionaryHeadwords( this, pDict );
        if ( scanPopup )
          scanPopup.get()->blockSignals( false );
      }
      else
      if( result && result == openDictFolderAction )
      {
        openDictionaryFolder( id );
      }
      else
      if( result && result == editAction )
      {
        editDictionary( pDict );
      }
    }
  }
}

void MainWindow::sendWordToInputLine( const QString & word )
{
  setTranslateBoxTextAndClearSuffix( word, EscapeWildcards, NoPopupChange );
}

void MainWindow::storeResourceSavePath( const QString & newPath )
{
  cfg.resourceSavePath = newPath;
}

void MainWindow::proxyAuthentication( const QNetworkProxy &,
                                      QAuthenticator * authenticator )
{
  QNetworkProxy proxy = QNetworkProxy::applicationProxy();

  QString * userStr, * passwordStr;
  if( cfg.preferences.proxyServer.useSystemProxy )
  {
    userStr = &cfg.preferences.proxyServer.systemProxyUser;
    passwordStr = &cfg.preferences.proxyServer.systemProxyPassword;
  }
  else
  {
    userStr = &cfg.preferences.proxyServer.user;
    passwordStr = &cfg.preferences.proxyServer.password;
  }

  if( proxy.user().isEmpty() && !userStr->isEmpty() )
  {
    authenticator->setUser( *userStr );
    authenticator->setPassword( *passwordStr );

    proxy.setUser( *userStr );
    proxy.setPassword( *passwordStr );
    QNetworkProxy::setApplicationProxy( proxy );
  }
  else
  {
    QDialog dlg;
    Ui::Dialog ui;
    ui.setupUi( &dlg );
    dlg.adjustSize();

    ui.userEdit->setText( *userStr );
    ui.passwordEdit->setText( *passwordStr );

    if ( dlg.exec() == QDialog::Accepted )
    {
      *userStr = ui.userEdit->text();
      *passwordStr = ui.passwordEdit->text();

      authenticator->setUser( *userStr );
      authenticator->setPassword( *passwordStr );

      proxy.setUser( *userStr );
      proxy.setPassword( *passwordStr );
      QNetworkProxy::setApplicationProxy( proxy );
    }
  }
}

void MainWindow::showFullTextSearchDialog()
{
  if( !ftsDlg )
  {
    ftsDlg = new FTS::FullTextSearchDialog( this, cfg, dictionaries, groupInstances, ftsIndexing );
    addGlobalActionsToDialog( ftsDlg );
    addGroupComboBoxActionsToDialog( ftsDlg, groupList );

    connect( ftsDlg, SIGNAL( showTranslationFor( QString, QStringList, QRegExp, bool ) ),
             this, SLOT( showTranslationFor( QString, QStringList, QRegExp, bool ) ) );
    connect( ftsDlg, SIGNAL( closeDialog() ),
             this, SLOT( closeFullTextSearchDialog() ), Qt::QueuedConnection );
    connect( &configEvents, SIGNAL( mutedDictionariesChanged() ),
             ftsDlg, SLOT( updateDictionaries() ) );

    unsigned group = groupInstances.empty() ? 0
                                            : groupInstances[ groupList->currentIndex() ].id;
    ftsDlg->setCurrentGroup( group );
  }

  if( !ftsDlg ->isVisible() )
    ftsDlg->show();
  else
  {
    ftsDlg->activateWindow();
    if ( headwordsDlg )
      headwordsDlg->lower();
  }
}

void MainWindow::closeFullTextSearchDialog()
{
  if( ftsDlg )
  {
    ftsDlg->stopSearch();
    delete ftsDlg;
    ftsDlg = 0;
  }
}

void MainWindow::showGDHelp()
{
  if( !helpWindow )
  {
    helpWindow = new Help::HelpWindow( this, cfg );

    if( helpWindow->getHelpEngine() )
    {
      connect( helpWindow, SIGNAL( needClose() ), this, SLOT( hideGDHelp() ) );
      helpWindow->showHelpFor( "Content" );
      helpWindow->show();
    }
    else
    {
      delete helpWindow;
      helpWindow = 0;
    }
  }
  else
  {
    helpWindow->show();
    helpWindow->activateWindow();
  }
}

void MainWindow::hideGDHelp()
{
  if( helpWindow )
    helpWindow->hide();
}

void MainWindow::showGDHelpForID( QString const & id )
{
  if( !helpWindow )
    showGDHelp();

  if( helpWindow )
  {
    helpWindow->showHelpFor( id );
    if( !helpWindow->isVisible() )
    {
      helpWindow->show();
      helpWindow->activateWindow();
    }
  }
}

void MainWindow::closeGDHelp()
{
  if( helpWindow )
  {
    delete helpWindow;
    helpWindow = 0;
  }
}

void MainWindow::showFTSIndexingName( QString const & name )
{
  if( name.isEmpty() )
    mainStatusBar->setBackgroundMessage( QString() );
  else
    mainStatusBar->setBackgroundMessage( tr( "Now indexing for full-text search: " ) + name );
}

QString MainWindow::unescapeTabHeader(QString const & header )
{
  // Reset table header to original headword

  QString escaped = header;
  escaped.replace( "&&", "&" );
  if( escaped.startsWith( QChar( 0x202E ) ) )
    escaped = escaped.mid( 1 );
  if( escaped.endsWith( QChar( 0x202C ) ) )
    escaped.chop( 1 );

  return escaped;
}

void MainWindow::addCurrentTabToFavorites()
{
  QString folder;
  Instances::Group const * igrp = groupInstances.findGroup( cfg.lastMainGroupId );
  if( igrp )
    folder = igrp->favoritesFolder;

  QString headword = ui.tabWidget->tabText( ui.tabWidget->currentIndex() );

  ui.favoritesPaneWidget->addHeadword( folder, unescapeTabHeader( headword ) );

  addToFavorites->setIcon( blueStarIcon );
  addToFavorites->setToolTip( tr( "Remove current tab from Favorites" ) );
}

void MainWindow::handleAddToFavoritesButton()
{
  QString folder;
  Instances::Group const * igrp = groupInstances.findGroup( cfg.lastMainGroupId );
  if( igrp )
    folder = igrp->favoritesFolder;
  QString headword = unescapeTabHeader( ui.tabWidget->tabText( ui.tabWidget->currentIndex() ) );

  if( ui.favoritesPaneWidget->isHeadwordPresent( folder, headword ) )
  {
    QMessageBox mb( QMessageBox::Question, "GoldenDict", tr( "Remove headword \"%1\" from Favorites?" ).arg( headword ),
                    QMessageBox::Yes | QMessageBox::No, this );
    if( mb.exec() == QMessageBox::Yes )
    {
      if( ui.favoritesPaneWidget->removeHeadword( folder, headword ) )
      {
        addToFavorites->setIcon( starIcon );
        addToFavorites->setToolTip( tr( "Add current tab to Favorites" ) );
      }
    }
  }
  else
  {
    ui.favoritesPaneWidget->addHeadword( folder, headword );
    addToFavorites->setIcon( blueStarIcon );
    addToFavorites->setToolTip( tr( "Remove current tab from Favorites" ) );
  }
}

void MainWindow::addWordToFavorites( QString const & word, unsigned groupId )
{
  QString folder;
  Instances::Group const * igrp = groupInstances.findGroup( groupId );
  if( igrp )
    folder = igrp->favoritesFolder;

  ui.favoritesPaneWidget->addHeadword( folder, word );
}

void MainWindow::addAllTabsToFavorites()
{
  QString folder;
  Instances::Group const * igrp = groupInstances.findGroup( cfg.lastMainGroupId );
  if( igrp )
    folder = igrp->favoritesFolder;

  for( int i = 0; i < ui.tabWidget->count(); i++ )
  {
    QString headword = ui.tabWidget->tabText( i );
    ui.favoritesPaneWidget->addHeadword( folder, unescapeTabHeader( headword ) );
  }
  addToFavorites->setIcon( blueStarIcon );
  addToFavorites->setToolTip( tr( "Remove current tab from Favorites" ) );
}

bool MainWindow::isWordPresentedInFavorites( QString const & word, unsigned groupId )
{
  QString folder;
  Instances::Group const * igrp = groupInstances.findGroup( groupId );
  if( igrp )
    folder = igrp->favoritesFolder;

  return ui.favoritesPaneWidget->isHeadwordPresent( folder, word );
}

void MainWindow::setGroupByName( QString const & name, bool main_window )
{
  if( main_window )
  {
    int i;
    for( i = 0; i < groupList->count(); i++ )
    {
      if( groupList->itemText( i ) == name )
      {
        groupList->setCurrentIndex( i );
        break;
      }
    }
    if( i >= groupList->count() )
      gdWarning( "Group \"%s\" for main window is not found\n", name.toUtf8().data() );
  }
  else
  {
    emit setPopupGroupByName( name );
  }
}

void MainWindow::headwordFromFavorites( QString const & headword,
                                        QString const & favoritesFolder )
{
  if( !favoritesFolder.isEmpty() )
  {
    // Find group by it Favorites folder
    for( Instances::Groups::size_type i = 0; i < groupInstances.size(); i++ )
    {
      if( groupInstances[ i ].favoritesFolder == favoritesFolder )
      {
        // Group found. Select it and stop search.
        if( groupList->currentIndex() != (int)i )
        {
          groupList->setCurrentIndex( i );

          // Restore focus on Favorites tree
          ui.favoritesPaneWidget->setFocusOnTree();
        }
        break;
      }
    }
  }

  // Show headword without lost of focus on Favorites tree
  setTranslateBoxTextAndClearSuffix( headword, EscapeWildcards, DisablePopup );
  showTranslationFor(headword );
}

#ifdef Q_OS_WIN32

bool MainWindow::handleGDMessage( MSG * message, long * result )
{
  if( message->message != gdAskMessage )
    return false;
  *result = 0;

  // TODO (Qt WebEngine): re-enable this code if ArticleView::wordAtPoint() is ported.
#ifdef USE_QTWEBKIT
  if( !isGoldenDictWindow( message->hwnd ) )
    return true;

  ArticleView * view = getCurrentArticleView();
  if( !view )
    return true;

  LPGDDataStruct lpdata = ( LPGDDataStruct ) message->lParam;

  QString str = view->wordAtPoint( lpdata->Pt.x, lpdata->Pt.y );

  memset( lpdata->cwData, 0, lpdata->dwMaxLength * sizeof( WCHAR ) );
  str.truncate( lpdata->dwMaxLength - 1 );
  str.toWCharArray( lpdata->cwData );

  *result = 1;
#endif // USE_QTWEBKIT
  return true;
}

bool MainWindow::isGoldenDictWindow( HWND hwnd )
{
  return hwnd == (HWND)winId() || hwnd == (HWND)ui.centralWidget->winId();
}

#endif

#include "mainwindow.moc"
