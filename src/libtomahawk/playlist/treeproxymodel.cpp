/* === This file is part of Tomahawk Player - <http://tomahawk-player.org> ===
 *
 *   Copyright 2010-2011, Christian Muehlhaeuser <muesli@tomahawk-player.org>
 *
 *   Tomahawk is free software: you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation, either version 3 of the License, or
 *   (at your option) any later version.
 *
 *   Tomahawk is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Tomahawk. If not, see <http://www.gnu.org/licenses/>.
 */

#include "treeproxymodel.h"

#include <QListView>

#include "source.h"
#include "query.h"
#include "database/database.h"
#include "database/databaseimpl.h"
#include "database/databasecommand_allalbums.h"
#include "utils/logger.h"


TreeProxyModel::TreeProxyModel( QObject* parent )
    : QSortFilterProxyModel( parent )
    , PlaylistInterface( this )
    , m_artistsFilterCmd( 0 )
    , m_model( 0 )
    , m_repeatMode( PlaylistInterface::NoRepeat )
    , m_shuffled( false )
{
    setFilterCaseSensitivity( Qt::CaseInsensitive );
    setSortCaseSensitivity( Qt::CaseInsensitive );
    setDynamicSortFilter( true );

    setSourceTreeModel( 0 );
}

QPersistentModelIndex
TreeProxyModel::currentIndex() const
{
    if ( !m_model )
        return QPersistentModelIndex();

    return mapFromSource( m_model->currentItem() );
}


void
TreeProxyModel::setSourceModel( QAbstractItemModel* sourceModel )
{
    Q_UNUSED( sourceModel );
    qDebug() << "Explicitly use setSourceTreeModel instead";
    Q_ASSERT( false );
}


void
TreeProxyModel::setSourceTreeModel( TreeModel* sourceModel )
{
    m_model = sourceModel;

    if ( m_model )
    {
        if ( m_model->metaObject()->indexOfSignal( "trackCountChanged(uint)" ) > -1 )
            connect( m_model, SIGNAL( trackCountChanged( unsigned int ) ), SIGNAL( sourceTrackCountChanged( unsigned int ) ) );

        connect( m_model, SIGNAL( rowsInserted( QModelIndex, int, int ) ), SLOT( onRowsInserted( QModelIndex, int, int ) ) );
        connect( m_model, SIGNAL( modelReset() ), SLOT( onModelReset() ) );
    }

    QSortFilterProxyModel::setSourceModel( sourceModel );
}


void
TreeProxyModel::onRowsInserted( const QModelIndex& parent, int /* start */, int /* end */ )
{
    if ( m_filter.isEmpty() )
        return;
    if ( sender() != m_model )
        return;

    TreeModelItem* pi = m_model->itemFromIndex( m_model->index( parent.row(), 0, parent.parent() ) );
    if ( pi->artist().isNull() )
        return;

    DatabaseCommand_AllAlbums* cmd = new DatabaseCommand_AllAlbums( m_model->collection() );
    cmd->setArtist( pi->artist() );
    cmd->setFilter( m_filter );

    connect( cmd, SIGNAL( albums( QList<Tomahawk::album_ptr>, QVariant ) ),
                    SLOT( onFilterAlbums( QList<Tomahawk::album_ptr> ) ) );

    Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( cmd ) );
}


void
TreeProxyModel::onModelReset()
{
    m_cache.clear();
    m_artistsFilter.clear();
    m_albumsFilter.clear();
}


void
TreeProxyModel::setFilter( const QString& pattern )
{
    emit filteringStarted();

    m_filter = pattern;
    m_albumsFilter.clear();

    if ( m_artistsFilterCmd )
    {
        disconnect( m_artistsFilterCmd, SIGNAL( artists( QList<Tomahawk::artist_ptr> ) ),
                    this,                 SLOT( onFilterArtists( QList<Tomahawk::artist_ptr> ) ) );

        m_artistsFilterCmd = 0;
    }

    if ( m_filter.isEmpty() )
    {
        filterFinished();
    }
    else
    {
        DatabaseCommand_AllArtists* cmd = new DatabaseCommand_AllArtists( m_model->collection() );
        cmd->setFilter( pattern );
        m_artistsFilterCmd = cmd;

        connect( cmd, SIGNAL( artists( QList<Tomahawk::artist_ptr> ) ),
                        SLOT( onFilterArtists( QList<Tomahawk::artist_ptr> ) ) );

        Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( cmd ) );
    }
}


void
TreeProxyModel::onFilterArtists( const QList<Tomahawk::artist_ptr>& artists )
{
    bool finished = true;
    m_artistsFilter = artists;

    foreach ( const Tomahawk::artist_ptr& artist, artists )
    {
        QModelIndex idx = m_model->indexFromArtist( artist );
        if ( m_model->rowCount( idx ) )
        {
            finished = false;

            DatabaseCommand_AllAlbums* cmd = new DatabaseCommand_AllAlbums( m_model->collection() );
            cmd->setArtist( artist );
            cmd->setFilter( m_filter );

            connect( cmd, SIGNAL( albums( QList<Tomahawk::album_ptr>, QVariant ) ),
                            SLOT( onFilterAlbums( QList<Tomahawk::album_ptr> ) ) );

            Database::instance()->enqueue( QSharedPointer<DatabaseCommand>( cmd ) );
        }
    }

    if ( finished )
        filterFinished();
}


void
TreeProxyModel::onFilterAlbums( const QList<Tomahawk::album_ptr>& albums )
{
    foreach ( const Tomahawk::album_ptr& album, albums )
        m_albumsFilter << album->id();

    filterFinished();
}


void
TreeProxyModel::filterFinished()
{
    m_artistsFilterCmd = 0;
    PlaylistInterface::setFilter( m_filter );
    setFilterRegExp( m_filter );

    emit filterChanged( m_filter );
    emit trackCountChanged( trackCount() );
    emit filteringFinished();
}


bool
TreeProxyModel::filterAcceptsRow( int sourceRow, const QModelIndex& sourceParent ) const
{
    TreeModelItem* item = sourceModel()->itemFromIndex( sourceModel()->index( sourceRow, 0, sourceParent ) );
    Q_ASSERT( item );

    if ( m_model->mode() == Tomahawk::DatabaseMode && !item->result().isNull() )
    {
        QList< Tomahawk::result_ptr > rl = m_cache.values( sourceParent );
        foreach ( const Tomahawk::result_ptr& cachedResult, rl )
        {
            if ( cachedResult->track() == item->result()->track() &&
               ( cachedResult->albumpos() == item->result()->albumpos() || cachedResult->albumpos() == 0 ) )
            {
                return ( cachedResult.data() == item->result().data() );
            }
        }

        for ( int i = 0; i < sourceModel()->rowCount( sourceParent ); i++ )
        {
            if ( i == sourceRow )
                continue;

            TreeModelItem* ti = sourceModel()->itemFromIndex( sourceModel()->index( i, 0, sourceParent ) );

            if ( ti->name() == item->name() &&
               ( ti->result()->albumpos() == item->result()->albumpos() || ti->result()->albumpos() == 0 || item->result()->albumpos() == 0 ) )
            {
                if ( !item->result()->isOnline() && ti->result()->isOnline() )
                    return false;

                if ( !item->result()->collection()->source()->isLocal() && ti->result()->collection()->source()->isLocal() )
                    return false;
            }
        }
    }

    bool accepted = false;
    if ( m_filter.isEmpty() )
        accepted = true;
    else if ( !item->artist().isNull() )
        accepted = m_artistsFilter.contains( item->artist() );
    else if ( !item->album().isNull() )
        accepted = m_albumsFilter.contains( item->album()->id() );

    if ( !accepted )
    {
        QStringList sl = m_filter.split( " ", QString::SkipEmptyParts );
        foreach( const QString& s, sl )
        {
            if ( !item->name().contains( s, Qt::CaseInsensitive ) &&
                 !item->albumName().contains( s, Qt::CaseInsensitive ) &&
                 !item->artistName().contains( s, Qt::CaseInsensitive ) )
            {
                return false;
            }
        }
    }

    m_cache.insertMulti( sourceParent, item->result() );
    return true;
}


bool
TreeProxyModel::lessThan( const QModelIndex& left, const QModelIndex& right ) const
{
    TreeModelItem* p1 = sourceModel()->itemFromIndex( left );
    TreeModelItem* p2 = sourceModel()->itemFromIndex( right );

    if ( !p1 )
        return true;
    if ( !p2 )
        return false;

/*    if ( !p1->result().isNull() && p2->result().isNull() )
        return true;
    if ( p1->result().isNull() && !p2->result().isNull() )
        return false;*/

    unsigned int albumpos1 = 0;
    unsigned int albumpos2 = 0;
    if ( !p1->query().isNull() )
        albumpos1 = p1->query()->albumpos();
    if ( !p2->query().isNull() )
        albumpos2 = p2->query()->albumpos();
    if ( albumpos1 == 0 && !p1->result().isNull() )
        albumpos1 = p1->result()->albumpos();
    if ( albumpos2 == 0 && !p2->result().isNull() )
        albumpos2 = p2->result()->albumpos();

    const QString& lefts = textForItem( p1 );
    const QString& rights = textForItem( p2 );

    if ( albumpos1 != albumpos2 )
        return albumpos1 < albumpos2;

    if ( lefts == rights )
        return (qint64)&p1 < (qint64)&p2;

    return QString::localeAwareCompare( lefts, rights ) < 0;
}


void
TreeProxyModel::removeIndex( const QModelIndex& index )
{
    qDebug() << Q_FUNC_INFO;

    if ( !sourceModel() )
        return;
    if ( index.column() > 0 )
        return;

    sourceModel()->removeIndex( mapToSource( index ) );
}


void
TreeProxyModel::removeIndexes( const QList<QModelIndex>& indexes )
{
    if ( !sourceModel() )
        return;

    foreach( const QModelIndex& idx, indexes )
    {
        removeIndex( idx );
    }
}


bool
TreeProxyModel::hasNextItem()
{
    return !( siblingItem( 1, true ).isNull() );
}


Tomahawk::result_ptr
TreeProxyModel::siblingItem( int itemsAway )
{
    return siblingItem( itemsAway, false );
}


Tomahawk::result_ptr
TreeProxyModel::siblingItem( int itemsAway, bool readOnly )
{
    QModelIndex idx = currentIndex();
    if ( !idx.isValid() )
        return Tomahawk::result_ptr();

    if ( m_shuffled )
    {
        idx = index( qrand() % rowCount( idx.parent() ), 0, idx.parent() );
    }
    else
    {
        if ( m_repeatMode != PlaylistInterface::RepeatOne )
            idx = index( idx.row() + ( itemsAway > 0 ? 1 : -1 ), 0, idx.parent() );
    }

    if ( !idx.isValid() && m_repeatMode == PlaylistInterface::RepeatAll )
    {
        if ( itemsAway > 0 )
        {
            // reset to first item
            idx = index( 0, 0, currentIndex().parent() );
        }
        else
        {
            // reset to last item
            idx = index( rowCount( currentIndex().parent() ) - 1, 0, currentIndex().parent() );
        }
    }

    // Try to find the next available PlaylistItem (with results)
    while ( idx.isValid() )
    {
        TreeModelItem* item = itemFromIndex( mapToSource( idx ) );
        if ( item && !item->result().isNull() && item->result()->isOnline() )
        {
            qDebug() << "Next PlaylistItem found:" << item->result()->url();
            if ( !readOnly )
                setCurrentIndex( idx );
            return item->result();
        }

        idx = index( idx.row() + ( itemsAway > 0 ? 1 : -1 ), 0, idx.parent() );
    }

    if ( !readOnly )
        setCurrentIndex( QModelIndex() );
    return Tomahawk::result_ptr();
}


Tomahawk::result_ptr
TreeProxyModel::currentItem() const
{
    TreeModelItem* item = itemFromIndex( mapToSource( currentIndex() ) );
    if ( item && !item->result().isNull() && item->result()->isOnline() )
        return item->result();
    return Tomahawk::result_ptr();
}


QString
TreeProxyModel::textForItem( TreeModelItem* item ) const
{
    if ( !item )
        return QString();

    if ( !item->artist().isNull() )
    {
        return item->artist()->sortname();
    }
    else if ( !item->album().isNull() )
    {
        return DatabaseImpl::sortname( item->album()->name() );
    }
    else if ( !item->result().isNull() )
    {
        return DatabaseImpl::sortname( item->result()->track() );
    }
    else if ( !item->query().isNull() )
    {
        return item->query()->track();
    }

    return QString();
}
