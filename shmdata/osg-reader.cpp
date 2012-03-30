/*
 * Copyright (C) 2012 Nicolas Bouillot (http://www.nicolasbouillot.net)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 2.1
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "shmdata/osg-reader.h"
#include "shmdata/osg-reader_impl.h"

namespace shmdata
{

    OsgReader::OsgReader ()
	: impl_ (new OsgReader_impl())
    {//do nothing
    }

    void
    OsgReader::start (const std::string &socketPath)
    {
	impl_->start (socketPath);
    }

    osg::Texture2D* 
    OsgReader::getTexture()
    {
	return impl_->getTexture();
    }
    
    OsgReader::~OsgReader ()
    {
	delete impl_;
    }

    void
    OsgReader::setDebug(bool debug)
    {
	impl_->setDebug(debug);
    }
    
} //end namespace