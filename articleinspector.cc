#ifdef USE_QTWEBKIT

#include "articleinspector.hh"

#include <algorithm>

using std::list;

list< ArticleInspector * > ArticleInspector::openedInspectors;

ArticleInspector::ArticleInspector( Config::Class * cfg, QWidget* parent ) :
  QWebInspector( parent ),
  cfg( cfg )
{
  if ( cfg == NULL )
    throw exInit();
}

ArticleInspector::~ArticleInspector()
{
}

void ArticleInspector::beforeClosed()
{
  list< ArticleInspector * >::iterator itemIter = std::find( openedInspectors.begin(),
                                                             openedInspectors.end(), this );
  if ( itemIter != openedInspectors.end() )
  {
    openedInspectors.erase( itemIter );
    // Save geometry of the recent closed inspector window
    QByteArray geometry = saveGeometry();
    cfg->inspectorGeometry = geometry;
  }
}

void ArticleInspector::showEvent( QShowEvent * event )
{
  if ( openedInspectors.empty() )
  {
    // Restore geometry from config, if no inspector opened
    restoreGeometry( cfg->inspectorGeometry );
  }
  else
  {
    // Load geometry from first inspector opened
    ArticleInspector * p = openedInspectors.front();
    setGeometry( p->geometry() );
  }

  if( std::find( openedInspectors.begin(), openedInspectors.end(), this ) == openedInspectors.end() )
    openedInspectors.push_back( this );

  QWebInspector::showEvent( event );
}

#endif // USE_QTWEBKIT
