#include "mesh.h"
#include "gl/gltools.h"
#include "../nifmodel.h"

#include <QClipboard>
#include <QDialog>
#include <QFileDialog>
#include <QGridLayout>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <cfloat>
#include <iostream>
#include <sstream>
#include <string>


// Brief description is deliberately not autolinked to class Spell
/*! \file mesh.cpp
 * \brief Mesh spells
 *
 * All classes here inherit from the Spell class.
 */

//! Find shape data of triangle geometry
static QModelIndex getShape( const NifModel * nif, const QModelIndex & index )
{
	QModelIndex iShape = nif->getBlock( index );

	if ( nif->isNiBlock( iShape, "NiTriBasedGeomData" ) )
		iShape = nif->getBlock( nif->getParent( nif->getBlockNumber( iShape ) ) );

	if ( nif->isNiBlock( iShape, { "NiTriShape", "BSLODTriShape", "NiTriStrips" } ) )
		if ( nif->getBlock( nif->getLink( iShape, "Data" ), "NiTriBasedGeomData" ).isValid() )
			return iShape;


	return QModelIndex();
}

//! Find triangle geometry
/*!
 * Subtly different to getShape(); that requires
 * <tt>nif->getBlock( nif->getLink( getShape( nif, index ), "Data" ) );</tt>
 * to return the same result.
 */
static QModelIndex getTriShapeData( const NifModel * nif, const QModelIndex & index )
{
	QModelIndex iData = nif->getBlock( index );

	if ( nif->isNiBlock( index, { "NiTriShape", "BSLODTriShape" } ) )
		iData = nif->getBlock( nif->getLink( index, "Data" ) );

	if ( nif->isNiBlock( iData, "NiTriShapeData" ) )
		return iData;

	return QModelIndex();
}

//! Removes elements of the specified type from an array
template <typename T> static void removeFromArray( QVector<T> & array, QMap<quint16, bool> map )
{
	for ( int x = array.count() - 1; x >= 0; x-- ) {
		if ( !map.contains( x ) )
			array.remove( x );
	}
}

//! Removes waste vertices from the specified data and shape
static void removeWasteVertices( NifModel * nif, const QModelIndex & iData, const QModelIndex & iShape )
{
	try
	{
		// read the data

		QVector<Vector3> verts = nif->getArray<Vector3>( iData, "Vertices" );

		if ( !verts.count() ) {
			throw QString( Spell::tr( "No vertices" ) );
		}

		QVector<Vector3> norms = nif->getArray<Vector3>( iData, "Normals" );
		QVector<Color4> colors = nif->getArray<Color4>( iData, "Vertex Colors" );
		QList<QVector<Vector2> > texco;
		QModelIndex iUVSets = nif->getIndex( iData, "UV Sets" );

		for ( int r = 0; r < nif->rowCount( iUVSets ); r++ ) {
			texco << nif->getArray<Vector2>( iUVSets.child( r, 0 ) );

			if ( texco.last().count() != verts.count() )
				throw QString( Spell::tr( "UV array size differs" ) );
		}

		int numVerts = verts.count();

		if ( numVerts != nif->get<int>( iData, "Num Vertices" )
		     || ( norms.count() && norms.count() != numVerts )
		     || ( colors.count() && colors.count() != numVerts ) )
		{
			throw QString( Spell::tr( "Vertex array size differs" ) );
		}

		// detect unused vertices

		QMap<quint16, bool> used;

		QVector<Triangle> tris = nif->getArray<Triangle>( iData, "Triangles" );
		for ( const Triangle& tri : tris ) {
			for ( int t = 0; t < 3; t++ ) {
				used.insert( tri[t], true );
			}
		}

		QList<QVector<quint16> > strips;
		QModelIndex iPoints = nif->getIndex( iData, "Points" );

		for ( int r = 0; r < nif->rowCount( iPoints ); r++ ) {
			strips << nif->getArray<quint16>( iPoints.child( r, 0 ) );
			for ( const auto p : strips.last() ) {
				used.insert( p, true );
			}
		}

		// remove them

		Message::info( nullptr, Spell::tr( "Removed %1 vertices" ).arg( verts.count() - used.count() ) );

		if ( verts.count() == used.count() )
			return;

		removeFromArray( verts, used );
		removeFromArray( norms, used );
		removeFromArray( colors, used );

		for ( int c = 0; c < texco.count(); c++ )
			removeFromArray( texco[c], used );

		// adjust the faces

		QMap<quint16, quint16> map;
		quint16 y = 0;

		for ( quint16 x = 0; x < numVerts; x++ ) {
			if ( used.contains( x ) )
				map.insert( x, y++ );
		}

		QMutableVectorIterator<Triangle> itri( tris );

		while ( itri.hasNext() ) {
			Triangle & tri = itri.next();

			for ( int t = 0; t < 3; t++ ) {
				if ( map.contains( tri[t] ) )
					tri[t] = map[ tri[t] ];
			}

		}

		QMutableListIterator<QVector<quint16> > istrip( strips );

		while ( istrip.hasNext() ) {
			QVector<quint16> & strip = istrip.next();

			for ( int s = 0; s < strip.size(); s++ ) {
				if ( map.contains( strip[s] ) )
					strip[s] = map[ strip[s] ];
			}
		}

		// write back the data

		nif->setArray<Triangle>( iData, "Triangles", tris );

		for ( int r = 0; r < nif->rowCount( iPoints ); r++ )
			nif->setArray<quint16>( iPoints.child( r, 0 ), strips[r] );

		nif->set<int>( iData, "Num Vertices", verts.count() );
		nif->updateArray( iData, "Vertices" );
		nif->setArray<Vector3>( iData, "Vertices", verts );
		nif->updateArray( iData, "Normals" );
		nif->setArray<Vector3>( iData, "Normals", norms );
		nif->updateArray( iData, "Vertex Colors" );
		nif->setArray<Color4>( iData, "Vertex Colors", colors );

		for ( int r = 0; r < nif->rowCount( iUVSets ); r++ ) {
			nif->updateArray( iUVSets.child( r, 0 ) );
			nif->setArray<Vector2>( iUVSets.child( r, 0 ), texco[r] );
		}

		// process NiSkinData

		QModelIndex iSkinInst = nif->getBlock( nif->getLink( iShape, "Skin Instance" ), "NiSkinInstance" );

		QModelIndex iSkinData = nif->getBlock( nif->getLink( iSkinInst, "Data" ), "NiSkinData" );
		QModelIndex iBones = nif->getIndex( iSkinData, "Bone List" );

		for ( int b = 0; b < nif->rowCount( iBones ); b++ ) {
			QVector<QPair<int, float> > weights;
			QModelIndex iWeights = nif->getIndex( iBones.child( b, 0 ), "Vertex Weights" );

			for ( int w = 0; w < nif->rowCount( iWeights ); w++ ) {
				weights.append( QPair<int, float>( nif->get<int>( iWeights.child( w, 0 ), "Index" ), nif->get<float>( iWeights.child( w, 0 ), "Weight" ) ) );
			}

			for ( int x = weights.count() - 1; x >= 0; x-- ) {
				if ( !used.contains( weights[x].first ) )
					weights.remove( x );
			}

			QMutableVectorIterator<QPair<int, float> > it( weights );

			while ( it.hasNext() ) {
				QPair<int, float> & w = it.next();

				if ( map.contains( w.first ) )
					w.first = map[ w.first ];
			}

			nif->set<int>( iBones.child( b, 0 ), "Num Vertices", weights.count() );
			nif->updateArray( iWeights );

			for ( int w = 0; w < weights.count(); w++ ) {
				nif->set<int>( iWeights.child( w, 0 ), "Index", weights[w].first );
				nif->set<float>( iWeights.child( w, 0 ), "Weight", weights[w].second );
			}
		}

		// process NiSkinPartition

		QModelIndex iSkinPart = nif->getBlock( nif->getLink( iSkinInst, "Skin Partition" ), "NiSkinPartition" );

		if ( !iSkinPart.isValid() )
			iSkinPart = nif->getBlock( nif->getLink( iSkinData, "Skin Partition" ), "NiSkinPartition" );

		if ( iSkinPart.isValid() ) {
			nif->removeNiBlock( nif->getBlockNumber( iSkinPart ) );
			Message::warning( nullptr, Spell::tr( "The skin partition was removed, please regenerate it with the skin partition spell" ) );
		}
	}
	catch ( QString e )
	{
		Message::warning( nullptr, Spell::tr( "There were errors during the operation" ), e );
	}
}

//! Flip texture UV coordinates
class spFlipTexCoords final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Flip UV" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif->itemType( index ).toLower() == "texcoord" || nif->inherits( index, "NiTriBasedGeomData" );
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		QModelIndex idx = index;

		if ( nif->itemType( index ).toLower() != "texcoord" ) {
			idx = nif->getIndex( nif->getBlock( index ), "UV Sets" );
		}

		QMenu menu;
		static const char * const flipCmds[3] = { "S = 1.0 - S", "T = 1.0 - T", "S <=> T" };

		for ( int c = 0; c < 3; c++ )
			menu.addAction( flipCmds[c] );

		QAction * act = menu.exec( QCursor::pos() );

		if ( act ) {
			for ( int c = 0; c < 3; c++ ) {
				if ( act->text() == flipCmds[c] )
					flip( nif, idx, c );
			}

		}

		return index;
	}

	//! Flips UV data in a model index
	void flip( NifModel * nif, const QModelIndex & index, int f )
	{
		if ( nif->isArray( index ) ) {
			QModelIndex idx = index.child( 0, 0 );

			if ( idx.isValid() ) {
				if ( nif->isArray( idx ) )
					flip( nif, idx, f );
				else {
					QVector<Vector2> tc = nif->getArray<Vector2>( index );

					for ( int c = 0; c < tc.count(); c++ )
						flip( tc[c], f );

					nif->setArray<Vector2>( index, tc );
				}
			}
		} else {
			Vector2 v = nif->get<Vector2>( index );
			flip( v, f );
			nif->set<Vector2>( index, v );
		}
	}

	//! Flips UV data in a vector
	void flip( Vector2 & v, int f )
	{
		switch ( f ) {
		case 0:
			v[0] = 1.0 - v[0];
			break;
		case 1:
			v[1] = 1.0 - v[1];
			break;
		default:
			{
				float x = v[0];
				v[0] = v[1];
				v[1] = x;
			}
			break;
		}
	}
};

REGISTER_SPELL( spFlipTexCoords )

//! Flips triangle faces, individually or in the selected array
class spFlipFace final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Flip Face" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return ( nif->getValue( index ).type() == NifValue::tTriangle )
		       || ( nif->isArray( index ) && nif->getValue( index.child( 0, 0 ) ).type() == NifValue::tTriangle );
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		if ( nif->isArray( index ) ) {
			QVector<Triangle> tris = nif->getArray<Triangle>( index );

			for ( int t = 0; t < tris.count(); t++ )
				tris[t].flip();

			nif->setArray<Triangle>( index, tris );
		} else {
			Triangle t = nif->get<Triangle>( index );
			t.flip();
			nif->set<Triangle>( index, t );
		}

		return index;
	}
};

REGISTER_SPELL( spFlipFace )

//! Flips all faces of a triangle based mesh
class spFlipAllFaces final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Flip Faces" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return getTriShapeData( nif, index ).isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		QModelIndex iData = getTriShapeData( nif, index );

		QVector<Triangle> tris = nif->getArray<Triangle>( iData, "Triangles" );

		for ( int t = 0; t < tris.count(); t++ )
			tris[t].flip();

		nif->setArray<Triangle>( iData, "Triangles", tris );

		return index;
	}
};

REGISTER_SPELL( spFlipAllFaces )

//! Removes redundant triangles from a mesh
class spPruneRedundantTriangles final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Prune Triangles" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return getTriShapeData( nif, index ).isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		QModelIndex iData = getTriShapeData( nif, index );

		QList<Triangle> tris = nif->getArray<Triangle>( iData, "Triangles" ).toList();
		int cnt = 0;

		int i = 0;

		while ( i < tris.count() ) {
			const Triangle & t = tris[i];

			if ( t[0] == t[1] || t[1] == t[2] || t[2] == t[0] ) {
				tris.removeAt( i );
				cnt++;
			} else {
				i++;
			}
		}

		i = 0;

		while ( i < tris.count() ) {
			const Triangle & t = tris[i];

			int j = i + 1;

			while ( j < tris.count() ) {
				const Triangle & r = tris[j];

				if ( ( t[0] == r[0] && t[1] == r[1] && t[2] == r[2] )
				     || ( t[0] == r[1] && t[1] == r[2] && t[2] == r[0] )
				     || ( t[0] == r[2] && t[1] == r[0] && t[2] == r[1] ) )
				{
					tris.removeAt( j );
					cnt++;
				} else {
					j++;
				}
			}

			i++;
		}

		if ( cnt > 0 ) {
			Message::info( nullptr, Spell::tr( "Removed %1 triangles" ).arg( cnt ) );
			nif->set<int>( iData, "Num Triangles", tris.count() );
			nif->set<int>( iData, "Num Triangle Points", tris.count() * 3 );
			nif->updateArray( iData, "Triangles" );
			nif->setArray<Triangle>( iData, "Triangles", tris.toVector() );
		}

		return index;
	}
};

REGISTER_SPELL( spPruneRedundantTriangles )

//! Removes duplicate vertices from a mesh
class spRemoveDuplicateVertices final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Remove Duplicate Vertices" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return getShape( nif, index ).isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		try
		{
			QModelIndex iShape = getShape( nif, index );
			QModelIndex iData  = nif->getBlock( nif->getLink( iShape, "Data" ) );

			// read the data

			QVector<Vector3> verts = nif->getArray<Vector3>( iData, "Vertices" );

			if ( !verts.count() )
				throw QString( Spell::tr( "No vertices" ) );

			QVector<Vector3> norms = nif->getArray<Vector3>( iData, "Normals" );
			QVector<Color4> colors = nif->getArray<Color4>( iData, "Vertex Colors" );
			QList<QVector<Vector2> > texco;
			QModelIndex iUVSets = nif->getIndex( iData, "UV Sets" );

			for ( int r = 0; r < nif->rowCount( iUVSets ); r++ ) {
				texco << nif->getArray<Vector2>( iUVSets.child( r, 0 ) );

				if ( texco.last().count() != verts.count() )
					throw QString( Spell::tr( "UV array size differs" ) );
			}

			int numVerts = verts.count();

			if ( numVerts != nif->get<int>( iData, "Num Vertices" )
			     || ( norms.count() && norms.count() != numVerts )
			     || ( colors.count() && colors.count() != numVerts ) )
			{
				throw QString( Spell::tr( "Vertex array size differs" ) );
			}

			// detect the dublicates

			QMap<quint16, quint16> map;

			for ( int a = 0; a < numVerts; a++ ) {
				Vector3 v = verts[a];

				for ( int b = 0; b < a; b++ ) {
					if ( !( v == verts[b] ) )
						continue;

					if ( norms.count() && !( norms[a] == norms[b] ) )
						continue;

					if ( colors.count() && !( colors[a] == colors[b] ) )
						continue;

					int t = 0;

					for ( t = 0; t < texco.count(); t++ ) {
						if ( !( texco[t][a] == texco[t][b] ) )
							break;
					}

					if ( t < texco.count() )
						continue;

					map.insert( b, a );
				}
			}

			//qDebug() << QString( Spell::tr("detected % duplicates") ).arg( map.count() );

			// adjust the faces

			QVector<Triangle> tris = nif->getArray<Triangle>( iData, "Triangles" );
			QMutableVectorIterator<Triangle> itri( tris );

			while ( itri.hasNext() ) {
				Triangle & t = itri.next();

				for ( int p = 0; p < 3; p++ ) {
					if ( map.contains( t[p] ) )
						t[p] = map.value( t[p] );
				}

			}

			nif->setArray<Triangle>( iData, "Triangles", tris );

			QModelIndex iPoints = nif->getIndex( iData, "Points" );

			for ( int r = 0; r < nif->rowCount( iPoints ); r++ ) {
				QVector<quint16> strip = nif->getArray<quint16>( iPoints.child( r, 0 ) );
				QMutableVectorIterator<quint16> istrp( strip );

				while ( istrp.hasNext() ) {
					quint16 & p = istrp.next();

					if ( map.contains( p ) )
						p = map.value( p );
				}

				nif->setArray<quint16>( iPoints.child( r, 0 ), strip );
			}

			// finally, remove the now unused vertices

			removeWasteVertices( nif, iData, iShape );
		}
		catch ( QString e )
		{
			Message::warning( nullptr, Spell::tr( "There were errors during the operation" ), e );
		}

		return index;
	}
};

REGISTER_SPELL( spRemoveDuplicateVertices )

//! Removes unused vertices
class spRemoveWasteVertices final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Remove Unused Vertices" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return getShape( nif, index ).isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		QModelIndex iShape = getShape( nif, index );
		QModelIndex iData  = nif->getBlock( nif->getLink( iShape, "Data" ) );

		removeWasteVertices( nif, iData, iShape );

		return index;
	}
};

REGISTER_SPELL( spRemoveWasteVertices )

/*
 * spUpdateCenterRadius
 */
bool spUpdateCenterRadius::isApplicable( const NifModel * nif, const QModelIndex & index )
{
	return nif->getBlock( index, "NiGeometryData" ).isValid();
}

QModelIndex spUpdateCenterRadius::cast( NifModel * nif, const QModelIndex & index )
{
	QModelIndex iData = nif->getBlock( index );

	QVector<Vector3> verts = nif->getArray<Vector3>( iData, "Vertices" );

	if ( !verts.count() )
		return index;

	Vector3 center;
	float radius = 0.0f;

	/*
	    Oblivion and CT_volatile meshes require a
	    different center algorithm
	*/
	if ( ( ( nif->getVersionNumber() & 0x14000000 ) && ( nif->getUserVersion() == 11 ) )
	     || ( nif->get<ushort>( iData, "Consistency Flags" ) & 0x8000 ) )
	{
		/* is a Oblivion mesh! */
		float xMin( FLT_MAX ), xMax( -FLT_MAX );
		float yMin( FLT_MAX ), yMax( -FLT_MAX );
		float zMin( FLT_MAX ), zMax( -FLT_MAX );
		for ( const Vector3& v : verts ) {
			if ( v[0] < xMin )
				xMin = v[0];
			else if ( v[0] > xMax )
				xMax = v[0];

			if ( v[1] < yMin )
				yMin = v[1];
			else if ( v[1] > yMax )
				yMax = v[1];

			if ( v[2] < zMin )
				zMin = v[2];
			else if ( v[2] > zMax )
				zMax = v[2];
		}

		center = Vector3( xMin + xMax, yMin + yMax, zMin + zMax ) / 2;
	} else {
		for ( const Vector3& v : verts ) {
			center += v;
		}
		center /= verts.count();
	}

	float d;
	for ( const Vector3& v : verts ) {
		if ( ( d = ( center - v ).length() ) > radius )
			radius = d;
	}

	nif->set<Vector3>( iData, "Center", center );
	nif->set<float>( iData, "Radius", radius );

	return index;
}

REGISTER_SPELL( spUpdateCenterRadius )

//! Updates Bounds of BSTriShape
class spUpdateBounds final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Update Bounds" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & index ) override final
	{
		return nif->inherits( index, "BSTriShape" ) && nif->getIndex( index, "Vertex Data" ).isValid();
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & index ) override final
	{
		auto vertData = nif->getIndex( index, "Vertex Data" );

		// Retrieve the verts
		QVector<Vector3> verts;
		for ( int i = 0; i < nif->rowCount( vertData ); i++ ) {
			verts << nif->get<Vector3>( vertData.child( i, 0 ), "Vertex" );
		}

		if ( verts.isEmpty() )
			return index;

		// Creating a bounding sphere from the verts
		BoundSphere bounds = BoundSphere( verts );

		// Update the bounding sphere
		auto boundsIdx = nif->getIndex( index, "Bounding Sphere" );
		nif->set<Vector3>( boundsIdx, "Center", bounds.center );
		nif->set<float>( boundsIdx, "Radius", bounds.radius );

		return index;
	}
};

REGISTER_SPELL( spUpdateBounds )


class spUpdateAllBounds final : public Spell
{
public:
	QString name() const override final { return Spell::tr( "Update All Bounds" ); }
	QString page() const override final { return Spell::tr( "Batch" ); }

	bool isApplicable( const NifModel * nif, const QModelIndex & idx ) override final
	{
		if ( !nif || idx.isValid() )
			return false;

		if ( nif->getUserVersion2() == 130 )
			return true;

		return false;
	}

	QModelIndex cast( NifModel * nif, const QModelIndex & ) override final
	{
		QList<QPersistentModelIndex> indices;

		spUpdateBounds updBounds;

		for ( int n = 0; n < nif->getBlockCount(); n++ ) {
			QModelIndex idx = nif->getBlock( n );

			if ( updBounds.isApplicable( nif, idx ) )
				indices << idx;
		}

		for ( const QModelIndex& idx : indices ) {
			updBounds.castIfApplicable( nif, idx );
		}

		return QModelIndex();
	}
};

REGISTER_SPELL( spUpdateAllBounds )

class spCopyVertexData final : public Spell {
public:
	QString name() const override final { return Spell::tr( "Copy Vertex Data" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable(const NifModel * nif, const QModelIndex & index) override {
		return nif->isNiBlock( index, "NiSkinPartition" )/* && nif->getIndex( index, "Vertex Data" )*/;

/*		if ( nif->isArray( index ) ) {
			//Check if array is of fixed size
			NifItem * item = static_cast<NifItem *>( index.internalPointer() );
			bool static1 = true;
			bool static2 = true;

			if ( item->arr1().isEmpty() == false ) {
				item->arr1().toInt( &static1 );
			}

			if ( item->arr2().isEmpty() == false ) {
				item->arr2().toInt( &static2 );
			}

			//Leave this commented out until a way for static arrays to be initialized to the right size is created.
			//if ( static1 && static2 )
			//{
			//	//Neither arr1 or arr2 is a variable name
			//	return false;
			//}

			//One of arr1 or arr2 is a variable name so the array is dynamic
			return item->name() == "Vertex Data";
		}

		return false;*/
	}

	QModelIndex cast(NifModel * nif, const QModelIndex & index) override {
		std::printf("NiSkinPartition: %d %d \n", nif->getBlockNumber(index), index.isValid());
		std::cout.flush();

		auto iVertData = nif->getIndex( index, "Vertex Data" );
		std::printf("Vertex data: %d \n", iVertData.isValid());
		std::cout.flush();

		auto vertexFlags = nif->get<quint16>( index, "VF" );
		bool hasNormals = vertexFlags & 0x80;

		std::stringstream res;
		std::printf("Begin parsing vertex data \n");
		std::cout.flush();
		auto numVerts = nif->get<int>( index, "Data Size" ) / nif->get<int>( index, "Vertex Size" );
		std::printf("Vertex count: %d \n", numVerts);
		std::cout.flush();
		res << "[";
		for ( int i = 0; i < numVerts; i++ ) {
//			if (i >= 1000) break;
			if (i > 0) res << ",\n";

			auto idx = nif->index( i, 0, iVertData );

			Vector3	vert	= nif->get<Vector3>( idx, "Vertex" );
			Vector2	uv	= nif->get<HalfVector2>( idx, "UV" );
			Vector3	norm	= nif->get<ByteVector3>( idx, "Normal" );

			res << "{";
			res << "\"vertex\":[" << vert[0] << ",\t" << vert[1] << ",\t" << vert[2] << "],\t";
			if (hasNormals) {
				res << "\"normal\":[" << norm[0] << ",\t" << norm[1] << ",\t" << norm[2] << "],\t";
			}
			res << "\"uv\":[" << uv[0] << ",\t" << uv[1] << "]\t";
			res << "}";
		}
		res << "]";

		std::printf("Exporting serialized vertex data to clipboard. \n");
		std::cout.flush();

		auto clipboard = QGuiApplication::clipboard();
		clipboard->setText(res.str().data());

		return index;
	}
};

REGISTER_SPELL( spCopyVertexData )

class spPasteVertexData final : public Spell {
public:
	QString name() const override final { return Spell::tr( "Paste Vertex Data" ); }
	QString page() const override final { return Spell::tr( "Mesh" ); }

	bool isApplicable(const NifModel * nif, const QModelIndex & index) override {
		// TODO: Check the clipboard for the correct MIME-type.
		return nif->isNiBlock( index, "NiSkinPartition" )/* && nif->getIndex( index, "Vertex Data" )*/;
	}

	QModelIndex cast(NifModel * nif, const QModelIndex & index) override {
		std::printf("NiSkinPartition: %d %d \n", nif->getBlockNumber(index), index.isValid());
		std::cout.flush();

		auto iVertData = nif->getIndex( index, "Vertex Data" );
		std::printf("Vertex data: %d \n", iVertData.isValid());
		std::cout.flush();

		auto vertexFlags = nif->get<quint16>( index, "VF" );
		bool hasNormals = vertexFlags & 0x80;

		std::stringstream res;
		auto numVerts = nif->get<int>( index, "Data Size" ) / nif->get<int>( index, "Vertex Size" );
		std::printf("Vertex count: %d \n", numVerts);
		std::printf("Begin de-serializing vertex data from clipboard \n");
		std::cout.flush();

		QVector<bool> used;
		QVector<Vector3> verts;
		QVector<Vector2> coords;
		QVector<Vector3> norms;

		auto clipboard = QGuiApplication::clipboard();
		auto vertexJson = clipboard->text();
		auto doc = QJsonDocument::fromJson(vertexJson.toUtf8());

		if (!doc.isArray()) {
			std::cerr << "Error reading JSON data from clipboard\n";
			std::cerr.flush();
			return index;
		}

		auto array = doc.array();
		auto arySize = array.size();
		std::printf("De-serialized vertex count: %d \n", arySize);
		if (arySize != numVerts) {
			std::cerr << "The imported array size is not equal to the vertex data size\n";
			std::cerr << "Was: " << arySize << ", Expected: " << numVerts << "\n";
			std::cerr.flush();
			return index;
		}

		for ( int i = 0; i < arySize; i++ ) {
			auto vertexObject = array[i].toObject();

			auto vertData = vertexObject["vertex"].toArray();
			verts += Vector3((float)vertData[0].toDouble(), (float)vertData[1].toDouble(), (float)vertData[2].toDouble());

			auto uvData = vertexObject["uv"].toArray();
			coords += HalfVector2((float)uvData[0].toDouble(), (float)uvData[1].toDouble());

			used += false;

			if (hasNormals && vertexObject["normal"].isArray()) {
				// TODO: Deserialize normals
			}
		}

		QVector<int> missedVertices;
		QVector<Vector3> missedVerticesV3;

		int modified = 0;
		int modifiedPrev;
		for (int i = 0; i < numVerts; i++) {
			modifiedPrev = modified;
			auto idx = nif->index( i, 0, iVertData );

			Vector3	vert	= nif->get<Vector3>( idx, "Vertex" );
			Vector2	uv	= nif->get<HalfVector2>( idx, "UV" );

			std::printf("%4d Vertex: %f %f %f\n", i, vert[0], vert[1], vert[2]);
			for ( int j = 0; j < arySize; j++ ) {
				if (used[j]) continue;
				// UV tolerance: 0.000001
				Vector2 uv2 = coords[j];
				float diffX = uv[0] - uv2[0];
				float diffY = uv[1] - uv2[1];
//				std::printf("%4d  UV: %f %f | %4d  UV2: %f %f\n", i, uv[0], uv[1], j, uv2[0], uv2[1]);
				if (diffX > -0.00001 && diffX < 0.00001 &&
					diffY > -0.00001 && diffY < 0.00001) {
					Vector3 vert2 = verts[j];
					std::printf("%4d   Match found (%4d %f %f %f)\n", i, j, vert2[0], vert2[1], vert2[2]);
					vert[0] = vert2[0];
					vert[1] = vert2[1];
					vert[2] = vert2[2];
					used[j] = true;
					modified++;
					break;
				}
			}
			if (modified == modifiedPrev) {
				std::printf("%4d   Match not found, vertex not modified!\n", i);
				missedVertices += i;
				missedVerticesV3 += vert;
			}
			std::cout.flush();

			nif->set<Vector3>( idx, "Vertex", vert );
			nif->set<HalfVector2>( idx, "UV", uv );
		}

		int missed = numVerts - modified;
		std::printf("Modified %d out of %d vertices.\n", modified, numVerts);
		std::cout.flush();
		if (missed > 0) {
			std::cerr << "Couldn’t paste " << missed << " vertices, needs manual fixing.\n";
			std::cerr << "Missed vertices:\n";
			for (int i = 0; i < missedVertices.size(); i++) {
				Vector3	vert = missedVerticesV3[i];
				std::cerr << missedVertices[i] << " (" << vert[0] << ", " << vert[1] << ", " << vert[2] << ")\n";
			}
			std::cerr.flush();
		}

		return index;
	}
};

REGISTER_SPELL( spPasteVertexData )

/*class spFixBodyMesh final : public Spell {
public:
	QString name() const override final { return Spell::tr( "Fix Body Mesh" ); };
	QString page() const override final { return Spell::tr( "Mesh" ); };

	bool isApplicable(const NifModel * nif, const QModelIndex & index) override {
		return nif->isNiBlock( index, "BSTriShape" ); // TODO: Make this better
	}

	QModelIndex cast(NifModel * nif, const QModelIndex & index) override {
		std::printf("Nif index: %d %d \n", nif->getBlockNumber(index), index.isValid());
		QVector<Vector3> verts;
		QVector<Vector3> norms;
		QVector<Vector2> texco;

		auto bsDismemberSkinInstance = nif->getIndex( index, "BSDismemberSkinInstance" );
		std::printf("BSDismemberSkinInstance: %d \n", bsDismemberSkinInstance.isValid());
		auto skinPartition = nif->getIndex( index, "NiSkinPartition" ); //
		std::printf("NiSkinPartition: %d \n", skinPartition.isValid());
		auto vertData = nif->getIndex( index, "Vertex Data" );
		std::printf("Vertex data: %d \n", vertData.isValid());
		std::cout.flush();

/ *		QString otherNifPath = QFileDialog::getOpenFileName( Q_NULLPTR, tr("Open File"), nif->getFileInfo().dir().path(), "Model (.nif)" );
		bool result = nif->loadFromFile(otherNifPath);
		if (!result) {
			// The other model didn’t load.
			return index;
		}

		QPersistentModelIndex iShape = index;
		QModelIndex iData;

		iData = nif->getBlock( nif->getLink( iShape, "Data" ) );

		QVector<Vector3> verts;
		QVector<Vector3> norms;
		QVector<Vector2> texco;* /

		return index;
	}
};

REGISTER_SPELL( spFixBodyMesh )*/
